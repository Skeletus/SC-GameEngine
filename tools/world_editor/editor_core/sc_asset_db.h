#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "world_format.h"

namespace sc
{
namespace editor
{
  enum class AssetType
  {
    Unknown,
    Model,
    Texture,
    Shader,
    World
  };

  enum class AssetStatus
  {
    Discovered,
    Indexed,
    Missing
  };

  struct AssetEntry
  {
    sc_world::AssetId id = 0;
    AssetType type = AssetType::Unknown;
    std::string relPath;
    std::string absPath;
    uint64_t fileSize = 0;
    uint64_t lastWriteTime = 0;
    AssetStatus status = AssetStatus::Discovered;
  };

  struct AssetFolder
  {
    std::string name;
    std::string relPath;
    int parent = -1;
    std::vector<int> children;
  };

  class AssetDatabase
  {
  public:
    void setRoot(const std::filesystem::path& path);
    const std::filesystem::path& root() const;
    bool hasValidRoot() const;

    void scanAll();
    void scanIncremental();

    const std::vector<AssetEntry>& getAll() const;
    std::vector<const AssetEntry*> getByType(AssetType type) const;
    const AssetEntry* findById(sc_world::AssetId id) const;
    std::vector<const AssetEntry*> searchByName(const std::string& substr) const;

    const std::vector<AssetFolder>& getFolders() const;
    int findFolderIndex(const std::string& relPath) const;

  private:
    void clear();
    void addFolder(const std::filesystem::path& absPath);
    void rebuildFolderOrder();
    static AssetType detectType(const std::filesystem::path& path);
    static uint64_t toUnixTime(const std::filesystem::file_time_type& ft);

    std::filesystem::path m_root;
    bool m_rootValid = false;
    std::vector<AssetEntry> m_entries;
    std::unordered_map<sc_world::AssetId, size_t> m_indexById;
    std::vector<AssetFolder> m_folders;
    std::unordered_map<std::string, int> m_folderIndex;
  };

  const char* AssetTypeLabel(AssetType type);
  const char* AssetStatusLabel(AssetStatus status);
  void requestLoadTexture(sc_world::AssetId id);
  void requestLoadModelGLB(sc_world::AssetId id);
}
}
