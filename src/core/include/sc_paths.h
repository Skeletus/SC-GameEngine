#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace sc
{
  std::filesystem::path exeDir();
  std::filesystem::path assetsRoot();
  std::filesystem::path resolveAssetPath(const std::filesystem::path& relativePath);
  std::string normalizePathForId(const std::filesystem::path& path);

  uint64_t fnv1a64(std::string_view text);
}
