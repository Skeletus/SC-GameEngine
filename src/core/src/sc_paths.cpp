#include "sc_paths.h"

#include <algorithm>
#include <array>
#include <cctype>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace sc
{
  std::filesystem::path exeDir()
  {
#if defined(_WIN32)
    std::array<char, 4096> buffer{};
    const DWORD len = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > 0 && len < buffer.size())
      return std::filesystem::path(buffer.data()).parent_path();
#endif
    return std::filesystem::current_path();
  }

  std::filesystem::path assetsRoot()
  {
    const std::filesystem::path exe = exeDir();
    const std::array<std::filesystem::path, 3> candidates =
    {
      exe / "assets",
      exe.parent_path() / "assets",
      exe.parent_path().parent_path() / "assets"
    };

    for (const auto& candidate : candidates)
    {
      if (std::filesystem::exists(candidate))
        return candidate;
    }

    return candidates[0];
  }

  std::filesystem::path resolveAssetPath(const std::filesystem::path& relativePath)
  {
    if (relativePath.is_absolute())
      return relativePath;

    const std::filesystem::path exe = exeDir();
    const std::array<std::filesystem::path, 5> candidates =
    {
      assetsRoot() / relativePath,
      exe / relativePath,
      exe.parent_path() / relativePath,
      exe.parent_path().parent_path() / relativePath,
      std::filesystem::current_path() / relativePath
    };

    for (const auto& candidate : candidates)
    {
      if (std::filesystem::exists(candidate))
        return std::filesystem::weakly_canonical(candidate);
    }

    return candidates[0];
  }

  std::string normalizePathForId(const std::filesystem::path& path)
  {
    std::filesystem::path normalized = path.lexically_normal();
    std::string text = normalized.generic_string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });
    return text;
  }

  uint64_t fnv1a64(std::string_view text)
  {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : text)
    {
      hash ^= static_cast<uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    return hash;
  }
}
