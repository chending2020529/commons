#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include <spdlog/common.h>
#include <spdlog/logger.h>

namespace Logger
{

enum class RotationMode
{
  None,
  Size,
  Daily,
  DailyAndSize,
};

struct LoggerOptions
{
  std::string logger_name{"ros2_shared_logger"};
  std::string file_path{"/tmp/commons_shared.log"};
  std::string pattern{"[pid:%P tid:%t] [%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v"};
  spdlog::level::level_enum level{spdlog::level::info};
  spdlog::level::level_enum flush_level{spdlog::level::info};
  bool also_log_to_console{true};
  bool truncate_on_open{false};
  RotationMode rotation_mode{RotationMode::DailyAndSize};
  // 默认单个日志文件大小为 50MB，超过后自动切分到新日志文件。
  std::size_t max_file_size_bytes{50 * 1024 * 1024};
  // 默认不限制日志文件个数，设置为 0 表示不按数量清理旧日志。
  std::size_t max_files{0};
  // 默认按 2 天保留日志，超过保留天数的旧日志会自动删除。
  std::size_t retention_days{2};
};

std::shared_ptr<spdlog::logger> init(const std::string & logger_name);
std::shared_ptr<spdlog::logger> init(const LoggerOptions & options);
std::shared_ptr<spdlog::logger> get();
void shutdown();

inline std::shared_ptr<spdlog::logger> defaultLogger()
{
  return get();
}

}  // namespace Logger

#define COMMONS_LOGGER_CONCAT_INNER(a, b) a##b
#define COMMONS_LOGGER_CONCAT(a, b) COMMONS_LOGGER_CONCAT_INNER(a, b)

// 基础日志宏：`Log*` 使用默认 logger，`LogWithLogger*` 使用显式传入的 logger。
#define LogWithLogger(logger, level, ...) (logger)->log(level, __VA_ARGS__)
#define Log(level, ...) ::Logger::defaultLogger()->log(level, __VA_ARGS__)

#define LogWithLoggerTrace(logger, ...) LogWithLogger(logger, spdlog::level::trace, __VA_ARGS__)
#define LogWithLoggerDebug(logger, ...) LogWithLogger(logger, spdlog::level::debug, __VA_ARGS__)
#define LogWithLoggerInfo(logger, ...) LogWithLogger(logger, spdlog::level::info, __VA_ARGS__)
#define LogWithLoggerWarn(logger, ...) LogWithLogger(logger, spdlog::level::warn, __VA_ARGS__)
#define LogWithLoggerError(logger, ...) LogWithLogger(logger, spdlog::level::err, __VA_ARGS__)
#define LogWithLoggerCritical(logger, ...) LogWithLogger(logger, spdlog::level::critical, __VA_ARGS__)

#define LogTrace(...) Log(spdlog::level::trace, __VA_ARGS__)
#define LogDebug(...) Log(spdlog::level::debug, __VA_ARGS__)
#define LogInfo(...) Log(spdlog::level::info, __VA_ARGS__)
#define LogWarn(...) Log(spdlog::level::warn, __VA_ARGS__)
#define LogError(...) Log(spdlog::level::err, __VA_ARGS__)
#define LogCritical(...) Log(spdlog::level::critical, __VA_ARGS__)

// 条件日志宏：只有 condition 为真时才输出日志。
#define LogWithLoggerIf(logger, condition, level, ...) \
  do { \
    if (condition) { \
      (logger)->log(level, __VA_ARGS__); \
    } \
  } while (0)

#define LogIf(condition, level, ...) \
  do { \
    if (condition) { \
      ::Logger::defaultLogger()->log(level, __VA_ARGS__); \
    } \
  } while (0)

#define LogWithLoggerTraceIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::trace, __VA_ARGS__)
#define LogWithLoggerDebugIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::debug, __VA_ARGS__)
#define LogWithLoggerInfoIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::info, __VA_ARGS__)
#define LogWithLoggerWarnIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::warn, __VA_ARGS__)
#define LogWithLoggerErrorIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::err, __VA_ARGS__)
#define LogWithLoggerCriticalIf(logger, condition, ...) LogWithLoggerIf(logger, condition, spdlog::level::critical, __VA_ARGS__)

#define LogTraceIf(condition, ...) LogIf(condition, spdlog::level::trace, __VA_ARGS__)
#define LogDebugIf(condition, ...) LogIf(condition, spdlog::level::debug, __VA_ARGS__)
#define LogInfoIf(condition, ...) LogIf(condition, spdlog::level::info, __VA_ARGS__)
#define LogWarnIf(condition, ...) LogIf(condition, spdlog::level::warn, __VA_ARGS__)
#define LogErrorIf(condition, ...) LogIf(condition, spdlog::level::err, __VA_ARGS__)
#define LogCriticalIf(condition, ...) LogIf(condition, spdlog::level::critical, __VA_ARGS__)

// 单次日志宏：同一调用位置只输出一次，适合抑制重复日志。
#define LogWithLoggerOnce(logger, level, ...) \
  do { \
    static std::atomic<bool> COMMONS_LOGGER_CONCAT(_commons_logger_once_, __LINE__){false}; \
    if (!COMMONS_LOGGER_CONCAT(_commons_logger_once_, __LINE__).exchange(true)) { \
      (logger)->log(level, __VA_ARGS__); \
    } \
  } while (0)

#define LogOnce(level, ...) \
  do { \
    static std::atomic<bool> COMMONS_LOGGER_CONCAT(_commons_logger_once_, __LINE__){false}; \
    if (!COMMONS_LOGGER_CONCAT(_commons_logger_once_, __LINE__).exchange(true)) { \
      ::Logger::defaultLogger()->log(level, __VA_ARGS__); \
    } \
  } while (0)

#define LogWithLoggerTraceOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::trace, __VA_ARGS__)
#define LogWithLoggerDebugOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::debug, __VA_ARGS__)
#define LogWithLoggerInfoOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::info, __VA_ARGS__)
#define LogWithLoggerWarnOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::warn, __VA_ARGS__)
#define LogWithLoggerErrorOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::err, __VA_ARGS__)
#define LogWithLoggerCriticalOnce(logger, ...) LogWithLoggerOnce(logger, spdlog::level::critical, __VA_ARGS__)

#define LogTraceOnce(...) LogOnce(spdlog::level::trace, __VA_ARGS__)
#define LogDebugOnce(...) LogOnce(spdlog::level::debug, __VA_ARGS__)
#define LogInfoOnce(...) LogOnce(spdlog::level::info, __VA_ARGS__)
#define LogWarnOnce(...) LogOnce(spdlog::level::warn, __VA_ARGS__)
#define LogErrorOnce(...) LogOnce(spdlog::level::err, __VA_ARGS__)
#define LogCriticalOnce(...) LogOnce(spdlog::level::critical, __VA_ARGS__)