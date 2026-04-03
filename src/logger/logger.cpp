#include "commons/logger/logger.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "commons/logger/multi_process_file_sink.hpp"

namespace Logger
{
namespace
{

std::mutex g_logger_mutex;
std::shared_ptr<spdlog::logger> g_logger;

std::string defaultFilePath()
{
  const char * env_value = std::getenv("ROS2_SHARED_LOG_FILE");
  if (env_value != nullptr && env_value[0] != '\0') {
    return env_value;
  }
  return "/tmp/ros2_shared.log";
}

LoggerOptions defaultOptions(const std::string & logger_name)
{
  LoggerOptions options;
  options.logger_name = logger_name;
  options.file_path = defaultFilePath();
  options.rotation_mode = RotationMode::DailyAndSize;
  return options;
}

std::shared_ptr<spdlog::logger> buildLogger(const LoggerOptions & options)
{
  LoggerOptions resolved = options;
  if (resolved.file_path.empty()) {
    resolved.file_path = defaultFilePath();
  }

  std::vector<spdlog::sink_ptr> sinks;
  sinks.push_back(std::make_shared<MultiProcessRollingFileSink>(resolved));

  if (resolved.also_log_to_console) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }

  auto logger = std::make_shared<spdlog::logger>(
    resolved.logger_name,
    sinks.begin(),
    sinks.end());

  logger->set_level(resolved.level);
  logger->set_pattern(resolved.pattern);
  logger->flush_on(resolved.flush_level);
  return logger;
}

}  // namespace

std::shared_ptr<spdlog::logger> init(const std::string & logger_name)
{
  return init(defaultOptions(logger_name));
}

std::shared_ptr<spdlog::logger> init(const LoggerOptions & options)
{
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  g_logger = buildLogger(options);
  spdlog::set_default_logger(g_logger);
  return g_logger;
}

std::shared_ptr<spdlog::logger> get()
{
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (!g_logger) {
    g_logger = buildLogger(defaultOptions("default_logger"));
    spdlog::set_default_logger(g_logger);
  }
  return g_logger;
}

void shutdown()
{
  std::lock_guard<std::mutex> lock(g_logger_mutex);
  if (g_logger) {
    g_logger->flush();
    spdlog::shutdown();
    g_logger.reset();
  }
}

}  // namespace Logger