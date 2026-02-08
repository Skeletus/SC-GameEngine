#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc_engine_render.h"
#include "world_format.h"
#include "mesh_importer.h"

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

  struct TextureRecord
  {
    sc_world::AssetId id = 0;
    ScRenderHandle handle = 0;
    uint64_t fileModifiedTime = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool srgb = true;
    uint64_t cpuBytes = 0;
    uint64_t gpuBytes = 0;
    bool resident = false;
    bool fromDisk = false;
  };

  class EditorTextureCache
  {
  public:
    void clear();
    const TextureRecord* find(sc_world::AssetId id) const;
    const TextureRecord* request(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);
    bool reload(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);
    ScRenderHandle resolveTextureHandle(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);

  private:
    TextureRecord* findMutable(sc_world::AssetId id);
    bool updateRecord(ScRenderContext* render, TextureRecord& record);

    std::unordered_map<sc_world::AssetId, TextureRecord> m_records;
  };

  struct ModelRecord
  {
    sc_world::AssetId id = 0;
    ScRenderHandle handle = 0;
    ScRenderMeshInfo meshInfo{};
    uint64_t fileModifiedTime = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t submeshCount = 0;
    uint32_t vertexLayoutFlags = 0;
    sc_world::AssetId defaultAlbedoTextureId = 0;
    bool loadFailed = false;
    std::string error;
    sc_import::MeshData previewMesh;
    sc_import::ImportedModel model;
  };

  class EditorModelCache
  {
  public:
    void clear();
    const ModelRecord* find(sc_world::AssetId id) const;
    const ModelRecord* request(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);
    bool reload(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);
    ScRenderHandle resolveMeshHandle(ScRenderContext* render, const AssetDatabase& db, sc_world::AssetId id);

  private:
    ModelRecord* findMutable(sc_world::AssetId id);
    bool ensureRegistry();
    bool loadRecord(ScRenderContext* render,
                    const AssetDatabase& db,
                    const AssetEntry& entry,
                    ModelRecord& record);
    sc_world::AssetId resolveTextureAssetId(const AssetDatabase& db,
                                            const AssetEntry& modelEntry,
                                            const std::string& uri) const;

    sc_import::ImporterRegistry m_registry;
    bool m_registryReady = false;
    std::unordered_map<sc_world::AssetId, ModelRecord> m_records;
  };

  const char* AssetTypeLabel(AssetType type);
  const char* AssetStatusLabel(AssetStatus status);
  void requestLoadTexture(sc_world::AssetId id);
  void requestLoadModelGLB(sc_world::AssetId id);
}
}
