#include "commons/utils/utils.h"

#include <cstdlib>
#include <filesystem>

namespace Commons
{
namespace Utils
{

std::string getEnvOr(const std::string & key, const std::string & fallback)
{
  const char * value = std::getenv(key.c_str());
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return value;
}

std::string joinPath(const std::vector<std::string> & parts)
{
  std::filesystem::path result;
  for (const auto & part : parts) {
    if (!part.empty()) {
      result /= part;
    }
  }
  return result.string();
}

std::optional<std::string> basename(const std::string & path)
{
  if (path.empty()) {
    return std::nullopt;
  }

  const auto name = std::filesystem::path(path).filename().string();
  if (name.empty()) {
    return std::nullopt;
  }
  return name;
}

}  // namespace Utils
}  // namespace Commons