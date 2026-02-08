#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sc_import
{
  enum VertexLayoutFlags : uint32_t
  {
    VertexLayout_Position = 1u << 0,
    VertexLayout_Normal = 1u << 1,
    VertexLayout_UV0 = 1u << 2
  };

  struct MeshVertex
  {
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float uv0[2] = { 0.0f, 0.0f };
  };

  struct MeshBounds
  {
    float min[3] = { 0.0f, 0.0f, 0.0f };
    float max[3] = { 0.0f, 0.0f, 0.0f };
    float center[3] = { 0.0f, 0.0f, 0.0f };
    float radius = 0.0f;
  };

  struct Submesh
  {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    int materialIndex = -1;
  };

  struct MeshData
  {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    uint32_t vertexLayoutFlags = VertexLayout_Position;
    MeshBounds bounds{};
    std::vector<Submesh> submeshes;
  };

  struct ImportedMaterial
  {
    std::string name;
    std::string baseColorTexture;
    bool baseColorTextureEmbedded = false;
  };

  struct ImportedMesh
  {
    std::string name;
    MeshData mesh;
  };

  struct ImportedNode
  {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    int meshIndex = -1;
    float localMatrix[16]{};
  };

  struct ImportedModel
  {
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedNode> nodes;
    std::vector<int> sceneRoots;
  };

  struct MeshImportOptions
  {
    bool bakeNodeTransforms = true;
  };

  class IMeshImporter
  {
  public:
    virtual ~IMeshImporter() = default;
    virtual bool canImportExtension(const char* extension) const = 0;
    virtual bool importFile(const char* absPath,
                            const MeshImportOptions& options,
                            ImportedModel* out_model,
                            std::string* out_error) = 0;
  };

  class ImporterRegistry
  {
  public:
    void registerImporter(std::unique_ptr<IMeshImporter> importer);
    bool importModel(const char* absPath,
                     const MeshImportOptions& options,
                     ImportedModel* out_model,
                     std::string* out_error) const;

  private:
    std::vector<std::unique_ptr<IMeshImporter>> m_importers;
  };

  bool FlattenModelToMesh(const ImportedModel& model, MeshData* out_mesh, std::string* out_error);
  void ComputeMeshBounds(MeshData* mesh);

  void RegisterGlbImporter(ImporterRegistry* registry);
}
