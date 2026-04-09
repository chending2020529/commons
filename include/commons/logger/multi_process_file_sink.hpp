#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

#include "commons/logger/logger.h"

namespace Logger {

class MultiProcessRollingFileSink
        : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit MultiProcessRollingFileSink(const LoggerOptions &options)
            : file_path_(options.file_path),
                truncate_on_open_(options.truncate_on_open),
                rotation_mode_(options.rotation_mode),
                max_file_size_bytes_(options.max_file_size_bytes),
                max_files_(options.max_files), retention_days_(options.retention_days),
                file_name_prefix_(options.file_name_prefix) {
        openLockFile();
        if (truncate_on_open_) {
            truncateActiveFiles();
            truncate_on_open_ = false;
        }
    }

    ~MultiProcessRollingFileSink() override {
        closeFile();
        closeLockFile();
    }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);

        if (::flock(lock_fd_, LOCK_EX) != 0) {
            throwSpdlogError("failed to acquire rotation lock");
        }

        const std::filesystem::path target_path =
                resolveTargetPath(formatted.size());
        if (current_file_path_ != target_path || fd_ < 0) {
            reopenFile(target_path);
        }

        writeFormatted(formatted);
        cleanupOldFiles();

        ::flock(lock_fd_, LOCK_UN);
    }

    void flush_() override {
        if (fd_ >= 0) {
            ::fsync(fd_);
        }
    }

private:
    struct ManagedFile {
        std::filesystem::path path;
        std::filesystem::file_time_type modified_at;
    };

    struct SessionMetadata {
        std::string active_file_key;
        std::string day_key;
    };

    static std::string currentDayKey() {
        const auto now = std::chrono::system_clock::now();
        const auto now_time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&now_time, &tm_buf);

        char buffer[16];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm_buf);
        return buffer;
    }

    static std::string currentTimestampKey() {
        const auto now = std::chrono::system_clock::now();
        const auto now_time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&now_time, &tm_buf);

        const auto milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) %
                1000;

        char buffer[40];
        char millis_buffer[8];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &tm_buf);
        std::snprintf(millis_buffer, sizeof(millis_buffer), "%03lld",
                                    static_cast<long long>(milliseconds.count()));
        return std::string(buffer) + "_" + millis_buffer;
    }

    static std::chrono::system_clock::time_point
    toSystemClock(const std::filesystem::file_time_type &file_time) {
        const auto adjusted = file_time -
                                                    std::filesystem::file_time_type::clock::now() +
                                                    std::chrono::system_clock::now();
        return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                adjusted);
    }

    bool useDailyRotation() const {
        return rotation_mode_ == RotationMode::Daily ||
                      rotation_mode_ == RotationMode::DailyAndSize;
    }

    bool useSizeRotation() const {
        return rotation_mode_ == RotationMode::Size ||
                      rotation_mode_ == RotationMode::DailyAndSize;
    }

    std::filesystem::path buildLogPath(const std::string &file_key) const {
        const auto parent = basePath().parent_path();
        const auto extension = basePath().extension().string().empty()
                                                              ? std::string(".log")
                                                              : basePath().extension().string();
        return parent / (file_name_prefix_ + file_key + extension);
    }

    std::filesystem::path metadataDirectory() const {
        const auto directory = basePath().parent_path() / ".meta";
        std::filesystem::create_directories(directory);
        return directory;
    }

    std::string metadataBaseName() const {
        return basePath().stem().string().empty() ? std::string("logger")
                                                                                            : basePath().stem().string();
    }

    std::filesystem::path resolveTargetPath(std::size_t incoming_size) {
        SessionMetadata metadata = resolveSessionMetadata();
        std::filesystem::path candidate = buildLogPath(metadata.active_file_key);

        if (!useSizeRotation()) {
            registerManagedFile(candidate);
            return candidate;
        }

        if (std::filesystem::exists(candidate)) {
            const auto size = std::filesystem::file_size(candidate);
            if (size + incoming_size > max_file_size_bytes_) {
                metadata.active_file_key = createUniqueFileKey();
                metadata.day_key = currentDayKey();
                writeSessionMetadata(metadata);
                candidate = buildLogPath(metadata.active_file_key);
            }
        }

        registerManagedFile(candidate);
        return candidate;
    }

    SessionMetadata resolveSessionMetadata() {
        SessionMetadata metadata = readSessionMetadata();
        const std::string day_key = currentDayKey();

        bool need_new_session = metadata.active_file_key.empty();
        if (!need_new_session && useDailyRotation() &&
                metadata.day_key != day_key) {
            need_new_session = true;
        }
        if (!need_new_session &&
                !std::filesystem::exists(buildLogPath(metadata.active_file_key))) {
            need_new_session = true;
        }

        if (need_new_session) {
            metadata.active_file_key = createUniqueFileKey();
            metadata.day_key = day_key;
            writeSessionMetadata(metadata);
            registerManagedFile(buildLogPath(metadata.active_file_key));
        }

        return metadata;
    }

    std::string createUniqueFileKey() const {
        for (;;) {
            const std::string candidate = currentTimestampKey();
            if (!std::filesystem::exists(buildLogPath(candidate))) {
                return candidate;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    SessionMetadata readSessionMetadata() const {
        SessionMetadata metadata;
        std::ifstream input(sessionMetadataPath());
        if (!input.is_open()) {
            return metadata;
        }

        std::getline(input, metadata.active_file_key);
        std::getline(input, metadata.day_key);
        return metadata;
    }

    void writeSessionMetadata(const SessionMetadata &metadata) const {
        std::ofstream output(sessionMetadataPath(), std::ios::trunc);
        if (!output.is_open()) {
            throwSpdlogError("failed to write session metadata");
        }
        output << metadata.active_file_key << '\n' << metadata.day_key << '\n';
    }

    std::vector<ManagedFile> matchingFiles() const {
        std::vector<ManagedFile> files;
        std::ifstream input(manifestPath());
        if (!input.is_open()) {
            return files;
        }

        std::vector<std::filesystem::path> unique_paths;
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) {
                continue;
            }

            const std::filesystem::path path(line);
            if (std::find(unique_paths.begin(), unique_paths.end(), path) !=
                    unique_paths.end()) {
                continue;
            }
            unique_paths.push_back(path);

            std::error_code ec;
            if (!std::filesystem::exists(path, ec) || ec) {
                continue;
            }
            files.push_back({path, std::filesystem::last_write_time(path, ec)});
            if (ec) {
                files.pop_back();
            }
        }

        return files;
    }

    void registerManagedFile(const std::filesystem::path &path) const {
        auto files = matchingFiles();
        const auto found =
                std::find_if(files.begin(), files.end(), [&](const ManagedFile &file) {
                    return file.path == path;
                });
        if (found != files.end()) {
            return;
        }

        std::ofstream output(manifestPath(), std::ios::app);
        if (!output.is_open()) {
            throwSpdlogError("failed to append manifest metadata");
        }
        output << path.string() << '\n';
    }

    void syncManagedFiles(const std::vector<ManagedFile> &files) const {
        std::ofstream output(manifestPath(), std::ios::trunc);
        if (!output.is_open()) {
            throwSpdlogError("failed to rewrite manifest metadata");
        }
        for (const auto &file : files) {
            output << file.path.string() << '\n';
        }
    }

    void writeFormatted(const spdlog::memory_buf_t &formatted) {
        if (::flock(fd_, LOCK_EX) != 0) {
            throwSpdlogError("failed to acquire file lock");
        }

        const char *data = formatted.data();
        size_t total = formatted.size();
        size_t written = 0;

        while (written < total) {
            const auto result = ::write(fd_, data + written, total - written);
            if (result < 0) {
                const int saved_errno = errno;
                ::flock(fd_, LOCK_UN);
                throwSpdlogError(std::string("failed to write log entry: ") +
                                                  std::strerror(saved_errno));
            }
            written += static_cast<size_t>(result);
        }

        ::flock(fd_, LOCK_UN);
    }

    void cleanupOldFiles() {
        auto files = matchingFiles();

        if (retention_days_ > 0) {
            const auto expiration_time = std::chrono::system_clock::now() -
                                                                      std::chrono::hours(24 * retention_days_);
            for (auto it = files.begin(); it != files.end();) {
                if (it->path == current_file_path_) {
                    ++it;
                    continue;
                }

                if (toSystemClock(it->modified_at) >= expiration_time) {
                    ++it;
                    continue;
                }

                std::error_code ec;
                if (std::filesystem::remove(it->path, ec)) {
                    it = files.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (max_files_ > 0 && files.size() > max_files_) {
            std::sort(files.begin(), files.end(),
                                [](const ManagedFile &lhs, const ManagedFile &rhs) {
                                    if (lhs.modified_at == rhs.modified_at) {
                                        return lhs.path.string() < rhs.path.string();
                                    }
                                    return lhs.modified_at < rhs.modified_at;
                                });

            std::size_t remaining = files.size();
            for (auto it = files.begin();
                      it != files.end() && remaining > max_files_;) {
                if (it->path == current_file_path_) {
                    ++it;
                    continue;
                }

                std::error_code ec;
                if (std::filesystem::remove(it->path, ec)) {
                    it = files.erase(it);
                    --remaining;
                } else {
                    ++it;
                }
            }
        }

        syncManagedFiles(files);
    }

    void truncateActiveFiles() {
        for (const auto &file : matchingFiles()) {
            std::error_code ec;
            std::filesystem::remove(file.path, ec);
        }

        SessionMetadata metadata = readSessionMetadata();
        if (!metadata.active_file_key.empty()) {
            std::error_code ec;
            std::filesystem::remove(buildLogPath(metadata.active_file_key), ec);
        }

        std::error_code metadata_ec;
        std::filesystem::remove(sessionMetadataPath(), metadata_ec);
        std::error_code manifest_ec;
        std::filesystem::remove(manifestPath(), manifest_ec);
        std::error_code lock_ec;
        std::filesystem::remove(lockFilePath(), lock_ec);
    }

    void reopenFile(const std::filesystem::path &path) {
        closeFile();
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd_ < 0) {
            throwSpdlogError(std::string("failed to open log file: ") +
                                              std::strerror(errno));
        }
        current_file_path_ = path;
    }

    void closeFile() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        current_file_path_.clear();
    }

    void openLockFile() {
        const auto meta_dir = metadataDirectory();
        const auto path = lockFilePath();
        (void)meta_dir;
        lock_fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY, 0644);
        if (lock_fd_ < 0) {
            throwSpdlogError(std::string("failed to open lock file: ") +
                                              std::strerror(errno));
        }
    }

    void closeLockFile() {
        if (lock_fd_ >= 0) {
            ::close(lock_fd_);
            lock_fd_ = -1;
        }
    }

    std::filesystem::path lockFilePath() const {
        return metadataDirectory() / (metadataBaseName() + ".lock");
    }

    std::filesystem::path sessionMetadataPath() const {
        return metadataDirectory() / (metadataBaseName() + ".session");
    }

    std::filesystem::path manifestPath() const {
        return metadataDirectory() / (metadataBaseName() + ".files");
    }

    const std::filesystem::path &basePath() const { return file_path_; }

    [[noreturn]] void throwSpdlogError(const std::string &message) const {
        throw spdlog::spdlog_ex(message + " [" + file_path_.string() + "]");
    }

    std::filesystem::path file_path_;
    bool truncate_on_open_;
    RotationMode rotation_mode_;
    std::size_t max_file_size_bytes_;
    std::size_t max_files_;
    std::size_t retention_days_;
    std::string file_name_prefix_;
    int fd_{-1};
    int lock_fd_{-1};
    std::filesystem::path current_file_path_;
};

} // namespace Logger
