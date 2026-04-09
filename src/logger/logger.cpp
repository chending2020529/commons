#include "commons/logger/logger.h"

#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
#include <filesystem>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "commons/logger/multi_process_file_sink.hpp"

namespace Logger
{
namespace
{

std::mutex g_logger_mutex;
std::shared_ptr<spdlog::logger> g_logger;

std::filesystem::path defaultLogDirectory()
{
    const char * home_env = std::getenv("HOME");
    std::filesystem::path home_path = (home_env != nullptr && home_env[0] != '\0')
        ? std::filesystem::path(home_env)
        : std::filesystem::current_path();

    const auto robot_dir = home_path / ".robot";
    const auto log_dir = robot_dir / "log";
    std::filesystem::create_directories(log_dir);
    return log_dir;
}

std::string defaultFilePath()
{
    const char * env_value = std::getenv("ROS2_SHARED_LOG_FILE");
    if (env_value != nullptr && env_value[0] != '\0') {
        return env_value;
    }
    return (defaultLogDirectory() / "commons_shared.log").string();
}

std::string defaultErrorFilePath(const std::string & file_path)
{
    const std::filesystem::path path(file_path);
    const auto parent = path.parent_path();
    const auto extension = path.extension().string().empty() ? std::string(".log") : path.extension().string();
    return (parent / ("Error" + extension)).string();
}

LoggerOptions defaultOptions(const std::string & logger_name)
{
    LoggerOptions options;
    options.logger_name = logger_name;
    options.file_path = defaultFilePath();
    options.error_file_path = defaultErrorFilePath(options.file_path);
    options.rotation_mode = RotationMode::DailyAndSize;
    return options;
}

std::shared_ptr<spdlog::logger> buildLogger(const LoggerOptions & options)
{
    LoggerOptions resolved = options;
    if (resolved.file_path.empty()) {
        resolved.file_path = defaultFilePath();
    }
    if (resolved.error_file_path.empty()) {
        resolved.error_file_path = defaultErrorFilePath(resolved.file_path);
    }

    std::vector<spdlog::sink_ptr> sinks;
    auto main_sink = std::make_shared<MultiProcessRollingFileSink>(resolved);
    main_sink->set_level(spdlog::level::trace);
    sinks.push_back(main_sink);

    LoggerOptions error_options = resolved;
    error_options.file_path = resolved.error_file_path;
    error_options.file_name_prefix = resolved.error_file_name_prefix;
    auto error_sink = std::make_shared<MultiProcessRollingFileSink>(error_options);
    error_sink->set_level(spdlog::level::err);
    sinks.push_back(error_sink);

    if (resolved.also_log_to_console) {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_level(spdlog::level::trace);
        sinks.push_back(console_sink);
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