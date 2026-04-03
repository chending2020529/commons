#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Commons
{
namespace Utils
{

std::string getEnvOr(const std::string & key, const std::string & fallback);
std::string joinPath(const std::vector<std::string> & parts);
std::optional<std::string> basename(const std::string & path);

}  // namespace Utils
}  // namespace Commons