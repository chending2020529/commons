#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include "commons/logger/logger.h"

namespace Logger
{

class MultiProcessRollingFileSink : public spdlog::sinks::base_sink<std::mutex>
{
public:
  explicit MultiProcessRollingFileSink(const LoggerOptions & options)
  : file_path_(options.file_path),
    truncate_on_open_(options.truncate_on_open),
    rotation_mode_(options.rotation_mode),
    max_file_size_bytes_(options.max_file_size_bytes),
    max_files_(options.max_files)
  {
    openLockFile();
    if (truncate_on_open_) {
      truncateActiveFiles();
      truncate_on_open_ = false;
    }
  }

  ~MultiProcessRollingFileSink() override
  {
    closeFile();
    closeLockFile();
  }

protected:
  void sink_it_(const spdlog::details::log_msg & msg) override
  {
    spdlog::memory_buf_t formatted;
    this->formatter_->format(msg, formatted);

    if (::flock(lock_fd_, LOCK_EX) != 0) {
      throwSpdlogError("failed to acquire rotation lock");
    }

    const std::filesystem::path target_path = resolveTargetPath(formatted.size());
    if (current_file_path_ != target_path || fd_ < 0) {
      reopenFile(target_path);
    }

    writeFormatted(formatted);
    cleanupOldFiles();

    ::flock(lock_fd_, LOCK_UN);
  }

  void flush_() override
  {
    if (fd_ >= 0) {
      ::fsync(fd_);
    }
  }

private:
  struct ManagedFile
  {
    std::filesystem::path path;
    std::filesystem::file_time_type modified_at;
  };

  struct SessionMetadata
  {
    std::string session_key;
    std::string day_key;
  };

  static std::string currentDayKey()
  {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_time, &tm_buf);

    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm_buf);
    return buffer;
  }

  static std::string currentTimestampKey()
  {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&now_time, &tm_buf);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &tm_buf);
    return buffer;
  }

  bool useDailyRotation() const
  {
    return rotation_mode_ == RotationMode::Daily || rotation_mode_ == RotationMode::DailyAndSize;
  }

  bool useSizeRotation() const
  {
    return rotation_mode_ == RotationMode::Size || rotation_mode_ == RotationMode::DailyAndSize;
  }

  std::filesystem::path buildRotatedPath(const std::string & session_key, std::size_t index) const
  {
    const auto parent = basePath().parent_path();
    const auto extension = basePath().extension().string().empty() ? std::string(".log") : basePath().extension().string();

    std::string filename = session_key;
    if (useSizeRotation() && index > 0) {
      filename += "." + std::to_string(index);
    }
    filename += extension;

    return parent / filename;
  }

  std::filesystem::path resolveTargetPath(std::size_t incoming_size)
  {
    const SessionMetadata session = resolveSessionMetadata();
    auto session_files = matchingFilesForSession(session.session_key);
    std::size_t max_index = 0;

    for (const auto & entry : session_files) {
      max_index = std::max(max_index, extractIndex(entry.path.filename().string(), session.session_key));
    }

    std::filesystem::path candidate = buildRotatedPath(session.session_key, max_index);
    if (!useSizeRotation()) {
      return candidate;
    }

    if (std::filesystem::exists(candidate)) {
      const auto size = std::filesystem::file_size(candidate);
      if (size + incoming_size > max_file_size_bytes_) {
        candidate = buildRotatedPath(session.session_key, max_index + 1);
      }
    }

    return candidate;
  }

  SessionMetadata resolveSessionMetadata()
  {
    SessionMetadata metadata = readSessionMetadata();
    const std::string day_key = currentDayKey();

    bool need_new_session = metadata.session_key.empty();
    if (!need_new_session && useDailyRotation() && metadata.day_key != day_key) {
      need_new_session = true;
    }
    if (!need_new_session && matchingFilesForSession(metadata.session_key).empty()) {
      need_new_session = true;
    }

    if (need_new_session) {
      metadata.session_key = currentTimestampKey();
      metadata.day_key = day_key;
      writeSessionMetadata(metadata);
    }

    return metadata;
  }

  SessionMetadata readSessionMetadata() const
  {
    SessionMetadata metadata;
    std::ifstream input(sessionMetadataPath());
    if (!input.is_open()) {
      return metadata;
    }

    std::getline(input, metadata.session_key);
    std::getline(input, metadata.day_key);
    return metadata;
  }

  void writeSessionMetadata(const SessionMetadata & metadata) const
  {
    std::ofstream output(sessionMetadataPath(), std::ios::trunc);
    if (!output.is_open()) {
      throwSpdlogError("failed to write session metadata");
    }
    output << metadata.session_key << '\n' << metadata.day_key << '\n';
  }

  std::vector<ManagedFile> matchingFiles() const
  {
    std::vector<ManagedFile> files;
    const auto parent = basePath().parent_path().empty() ? std::filesystem::path(".") : basePath().parent_path();
    if (!std::filesystem::exists(parent)) {
      return files;
    }

    for (const auto & entry : std::filesystem::directory_iterator(parent)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto filename = entry.path().filename().string();
      if (!isManagedFile(filename)) {
        continue;
      }
      files.push_back({entry.path(), entry.last_write_time()});
    }

    return files;
  }

  std::vector<ManagedFile> matchingFilesForSession(const std::string & session_key) const
  {
    std::vector<ManagedFile> files;
    for (const auto & file : matchingFiles()) {
      if (extractSessionKey(file.path.filename().string()) == session_key) {
        files.push_back(file);
      }
    }
    return files;
  }

  bool isManagedFile(const std::string & filename) const
  {
    const std::string extension = basePath().extension().string().empty() ? std::string(".log") : basePath().extension().string();
    if (filename.size() <= extension.size()) {
      return false;
    }
    if (filename.substr(filename.size() - extension.size()) != extension) {
      return false;
    }

    std::string body = filename.substr(0, filename.size() - extension.size());
    const auto suffix_pos = body.find_last_of('.');
    if (suffix_pos != std::string::npos) {
      const std::string suffix = body.substr(suffix_pos + 1);
      if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        body = body.substr(0, suffix_pos);
      }
    }

    return !body.empty();
  }

  std::string extractSessionKey(const std::string & filename) const
  {
    const std::string extension = basePath().extension().string().empty() ? std::string(".log") : basePath().extension().string();
    if (filename.size() <= extension.size() || filename.substr(filename.size() - extension.size()) != extension) {
      return {};
    }

    std::string body = filename.substr(0, filename.size() - extension.size());
    const auto suffix_pos = body.find_last_of('.');
    if (suffix_pos != std::string::npos) {
      const std::string suffix = body.substr(suffix_pos + 1);
      if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
        body = body.substr(0, suffix_pos);
      }
    }

    return body;
  }

  std::size_t extractIndex(const std::string & filename, const std::string & session_key) const
  {
    const std::string extension = basePath().extension().string().empty() ? std::string(".log") : basePath().extension().string();
    const std::string prefix = session_key;

    if (filename == prefix + extension) {
      return 0;
    }
    if (filename.rfind(prefix, 0) != 0) {
      return 0;
    }

    const std::string suffix = filename.substr(prefix.size(), filename.size() - prefix.size() - extension.size());
    if (suffix.empty() || suffix[0] != '.') {
      return 0;
    }

    return static_cast<std::size_t>(std::stoul(suffix.substr(1)));
  }

  void writeFormatted(const spdlog::memory_buf_t & formatted)
  {
    if (::flock(fd_, LOCK_EX) != 0) {
      throwSpdlogError("failed to acquire file lock");
    }

    const char * data = formatted.data();
    size_t total = formatted.size();
    size_t written = 0;

    while (written < total) {
      const auto result = ::write(fd_, data + written, total - written);
      if (result < 0) {
        const int saved_errno = errno;
        ::flock(fd_, LOCK_UN);
        throwSpdlogError(std::string("failed to write log entry: ") + std::strerror(saved_errno));
      }
      written += static_cast<size_t>(result);
    }

    ::flock(fd_, LOCK_UN);
  }

  void cleanupOldFiles()
  {
    if (max_files_ == 0) {
      return;
    }

    auto files = matchingFiles();
    if (files.size() <= max_files_) {
      return;
    }

    std::sort(files.begin(), files.end(), [](const ManagedFile & lhs, const ManagedFile & rhs) {
      if (lhs.modified_at == rhs.modified_at) {
        return lhs.path.string() < rhs.path.string();
      }
      return lhs.modified_at < rhs.modified_at;
    });

    std::size_t remaining = files.size();
    for (const auto & file : files) {
      if (remaining <= max_files_) {
        break;
      }
      if (file.path == current_file_path_) {
        continue;
      }
      std::error_code ec;
      if (std::filesystem::remove(file.path, ec)) {
        --remaining;
      }
    }
  }

  void truncateActiveFiles()
  {
    for (const auto & file : matchingFiles()) {
      std::error_code ec;
      std::filesystem::remove(file.path, ec);
    }
    std::error_code metadata_ec;
    std::filesystem::remove(sessionMetadataPath(), metadata_ec);
  }

  void reopenFile(const std::filesystem::path & path)
  {
    closeFile();
    const auto parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) {
      throwSpdlogError(std::string("failed to open log file: ") + std::strerror(errno));
    }
    current_file_path_ = path;
  }

  void closeFile()
  {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    current_file_path_.clear();
  }

  void openLockFile()
  {
    const auto parent = basePath().parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto path = lockFilePath();
    lock_fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    if (lock_fd_ < 0) {
      throwSpdlogError(std::string("failed to open lock file: ") + std::strerror(errno));
    }
  }

  void closeLockFile()
  {
    if (lock_fd_ >= 0) {
      ::close(lock_fd_);
      lock_fd_ = -1;
    }
  }

  std::filesystem::path lockFilePath() const
  {
    return basePath().parent_path() / (basePath().filename().string() + ".lock");
  }

  std::filesystem::path sessionMetadataPath() const
  {
    return basePath().parent_path() / (basePath().filename().string() + ".session");
  }

  const std::filesystem::path & basePath() const
  {
    return file_path_;
  }

  [[noreturn]] void throwSpdlogError(const std::string & message) const
  {
    throw spdlog::spdlog_ex(message + " [" + file_path_.string() + "]");
  }

  std::filesystem::path file_path_;
  bool truncate_on_open_;
  RotationMode rotation_mode_;
  std::size_t max_file_size_bytes_;
  std::size_t max_files_;
  int fd_{-1};
  int lock_fd_{-1};
  std::filesystem::path current_file_path_;
};

}  // namespace Logger
