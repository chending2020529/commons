# commons

`commons` 是一个通用的 `ament_cmake` 基础库功能包，不只承载日志能力，也可以继续沉淀 `utils` 等公共模块。

## 当前模块

- `logger/`: 基于 `spdlog` 的多进程共享日志模块
- `utils/`: 通用工具函数模块

## 目录结构

```text
commons/
  include/commons/
    commons.h
    logger/
      logger.h
      multi_process_file_sink.hpp
    utils/
      utils.h
  src/
    logger/
      logger.cpp
    utils/
      utils.cpp
```

## 使用方式

如果只用日志模块：

```cpp
#include "commons/logger/logger.h"
```

如果想使用整个公共库入口：

```cpp
#include "commons/commons.h"
```

## 日志最简用法

```cpp
int main(int argc, char ** argv)
{
  std::string nodeName = "multi_camera";
  Logger::init(nodeName);
  LogInfo("test");

  rclcpp::init(argc, argv);
  auto camera = std::make_shared<MultiCamera>();
  rclcpp::spin(camera);
  rclcpp::shutdown();

  Logger::shutdown();
  return 0;
}
```

## 指定 logger 实例

```cpp
auto logger = Logger::get();
LogWithLoggerInfo(logger, "hello from explicit logger");
```

## utils 示例

```cpp
#include "commons/utils/utils.h"

auto logDir = Commons::Utils::getEnvOr("ROS_LOG_DIR", "/tmp");
auto logPath = Commons::Utils::joinPath({logDir, "robot.log"});
```

## 测试功能包

- `camera_test_node`
- `perception_test_node`

两个测试包都会依赖 `commons`，并把日志写到同一个文件，用来验证多进程共享日志。

## 构建

```bash
colcon build --packages-select commons camera_test_node perception_test_node
```