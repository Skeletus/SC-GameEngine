#include "sc_asset_db.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace sc
{
namespace editor
{
  namespace
  {
    static std::string toLower(std::string text)
    {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
      {
        return static_cast<char>(std::tolower(c));
      });
      return text;
    }

    static bool isParentPathComponentValid(const std::filesystem::path& rel)
    {
      for (const auto& part : rel)
      {
        if (part == "..")
          return false;
      }
      return true;
    }
  }

  void AssetDatabase::setRoot(const std::filesystem::path& path)
  {
    if (path.empty())
    {
      m_root.clear();
      m_rootValid = false;
      return;
    }

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(path, ec);
    if (!ec)
      m_root = abs.lexically_normal();
    else
      m_root = path.lexically_normal();

    m_rootValid = std::filesystem::exists(m_root, ec) && std::filesystem::is_directory(m_root, ec);
  }

  const std::filesystem::path& AssetDatabase::root() const
  {
    return m_root;
  }

  bool AssetDatabase::hasValidRoot() const
  {
    return m_rootValid;
  }

  void AssetDatabase::clear()
  {
    m_entries.clear();
    m_indexById.clear();
    m_folders.clear();
    m_folderIndex.clear();
  }

  void AssetDatabase::scanAll()
  {
    clear();

    if (m_root.empty())
    {
      m_rootValid = false;
      return;
    }

    std::error_code ec;
    m_rootValid = std::filesystem::exists(m_root, ec) && std::filesystem::is_directory(m_root, ec);

    AssetFolder root_folder{};
    root_folder.relPath = "";
    root_folder.parent = -1;
    root_folder.name = m_root.filename().string();
    if (root_folder.name.empty())
      root_folder.name = m_root.string();
    if (root_folder.name.empty())
      root_folder.name = "assets";
    m_folders.push_back(root_folder);
    m_folderIndex[root_folder.relPath] = 0;

    if (!m_rootValid)
      return;

    std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(m_root, options, ec);
    std::filesystem::recursive_directory_iterator end;

    while (!ec && it != end)
    {
      const std::filesystem::directory_entry entry = *it;
      const std::filesystem::path path = entry.path();

      if (entry.is_directory(ec))
      {
        addFolder(path);
      }
      else if (entry.is_regular_file(ec))
      {
        const AssetType type = detectType(path);
        if (type == AssetType::Unknown)
        {
          ++it;
          continue;
        }

        std::filesystem::path rel = path.lexically_relative(m_root);
        if (rel.empty() || rel == "." || !isParentPathComponentValid(rel))
        {
          ++it;
          continue;
        }

        const std::string rel_path = rel.generic_string();

        AssetEntry asset{};
        asset.type = type;
        asset.relPath = rel_path;
        asset.absPath = path.string();
        asset.id = sc_world::HashAssetPath(rel_path.c_str());
        asset.status = AssetStatus::Indexed;

        std::error_code size_ec;
        const uintmax_t size = std::filesystem::file_size(path, size_ec);
        asset.fileSize = size_ec ? 0u : static_cast<uint64_t>(size);

        std::error_code time_ec;
        const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, time_ec);
        asset.lastWriteTime = time_ec ? 0u : toUnixTime(write_time);

        if (m_indexById.find(asset.id) == m_indexById.end())
        {
          m_indexById[asset.id] = m_entries.size();
          m_entries.push_back(std::move(asset));
        }
      }

      ++it;
    }

    rebuildFolderOrder();
  }

  void AssetDatabase::scanIncremental()
  {
    scanAll();
  }

  const std::vector<AssetEntry>& AssetDatabase::getAll() const
  {
    return m_entries;
  }

  std::vector<const AssetEntry*> AssetDatabase::getByType(AssetType type) const
  {
    std::vector<const AssetEntry*> out;
    for (const auto& entry : m_entries)
    {
      if (entry.type == type)
        out.push_back(&entry);
    }
    return out;
  }

  const AssetEntry* AssetDatabase::findById(sc_world::AssetId id) const
  {
    auto it = m_indexById.find(id);
    if (it == m_indexById.end())
      return nullptr;
    return &m_entries[it->second];
  }

  std::vector<const AssetEntry*> AssetDatabase::searchByName(const std::string& substr) const
  {
    std::vector<const AssetEntry*> out;
    if (substr.empty())
      return out;
    const std::string needle = toLower(substr);
    for (const auto& entry : m_entries)
    {
      const std::string hay = toLower(entry.relPath);
      if (hay.find(needle) != std::string::npos)
        out.push_back(&entry);
    }
    return out;
  }

  const std::vector<AssetFolder>& AssetDatabase::getFolders() const
  {
    return m_folders;
  }

  int AssetDatabase::findFolderIndex(const std::string& relPath) const
  {
    auto it = m_folderIndex.find(relPath);
    if (it == m_folderIndex.end())
      return -1;
    return it->second;
  }

  void AssetDatabase::addFolder(const std::filesystem::path& absPath)
  {
    if (m_root.empty())
      return;

    std::filesystem::path rel = absPath.lexically_relative(m_root);
    if (rel.empty() || rel == ".")
      return;
    if (!isParentPathComponentValid(rel))
      return;

    const std::string rel_path = rel.generic_string();
    if (m_folderIndex.find(rel_path) != m_folderIndex.end())
      return;

    std::filesystem::path parent_rel = rel.parent_path();
    std::string parent_path = parent_rel.empty() ? "" : parent_rel.generic_string();

    int parent_index = 0;
    if (!parent_path.empty())
    {
      addFolder(m_root / parent_rel);
      auto it = m_folderIndex.find(parent_path);
      if (it != m_folderIndex.end())
        parent_index = it->second;
    }

    AssetFolder folder{};
    folder.relPath = rel_path;
    folder.parent = parent_index;
    folder.name = absPath.filename().string();
    if (folder.name.empty())
      folder.name = rel_path;

    const int index = static_cast<int>(m_folders.size());
    m_folders.push_back(folder);
    m_folderIndex[rel_path] = index;
    if (parent_index >= 0 && parent_index < static_cast<int>(m_folders.size()))
      m_folders[parent_index].children.push_back(index);
  }

  void AssetDatabase::rebuildFolderOrder()
  {
    auto name_less = [&](int a, int b)
    {
      const std::string al = toLower(m_folders[a].name);
      const std::string bl = toLower(m_folders[b].name);
      return al < bl;
    };

    for (auto& folder : m_folders)
      std::sort(folder.children.begin(), folder.children.end(), name_less);
  }

  AssetType AssetDatabase::detectType(const std::filesystem::path& path)
  {
    std::string ext = path.extension().string();
    ext = toLower(ext);

    if (ext == ".glb" || ext == ".gltf")
      return AssetType::Model;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" || ext == ".ktx2")
      return AssetType::Texture;
    if (ext == ".vert" || ext == ".frag" || ext == ".comp")
      return AssetType::Shader;
    if (ext == ".scsector" || ext == ".scworld")
      return AssetType::World;
    return AssetType::Unknown;
  }

  uint64_t AssetDatabase::toUnixTime(const std::filesystem::file_time_type& ft)
  {
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
    return static_cast<uint64_t>(duration_cast<seconds>(sctp.time_since_epoch()).count());
  }

  const char* AssetTypeLabel(AssetType type)
  {
    switch (type)
    {
      case AssetType::Model: return "Model";
      case AssetType::Texture: return "Texture";
      case AssetType::Shader: return "Shader";
      case AssetType::World: return "World";
      case AssetType::Unknown: default: return "Unknown";
    }
  }

  const char* AssetStatusLabel(AssetStatus status)
  {
    switch (status)
    {
      case AssetStatus::Discovered: return "Discovered";
      case AssetStatus::Indexed: return "Indexed";
      case AssetStatus::Missing: return "Missing";
      default: return "Unknown";
    }
  }

  void requestLoadTexture(sc_world::AssetId id)
  {
    std::printf("[AssetDB] requestLoadTexture: 0x%016llX\n", static_cast<unsigned long long>(id));
  }

  void requestLoadModelGLB(sc_world::AssetId id)
  {
    std::printf("[AssetDB] requestLoadModelGLB: 0x%016llX\n", static_cast<unsigned long long>(id));
  }
}
}
