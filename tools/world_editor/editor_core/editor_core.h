#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sc_engine_render.h"
#include "asset_registry.h"
#include "world_format.h"
#include "sc_math.h"

namespace sc
{
namespace editor
{
  class AssetDatabase;
  class EditorTextureCache;
  class EditorModelCache;
  struct EditorTransform
  {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
  };

  struct EditorEntity
  {
    uint64_t id = 0;
    char name[64] = "Entity";
    EditorTransform transform{};
    sc_world::AssetId modelAssetId = 0;
    sc_world::AssetId meshAssetId = 0;
    sc_world::AssetId materialAssetId = 0;
    sc_world::AssetId albedoTextureAssetId = 0;
    bool useTexture = false;
    uint32_t tags = 0;

    ScRenderHandle meshHandle = 0;
    ScRenderHandle materialHandle = 0;
    ScRenderMeshInfo meshInfo{};
  };

  struct EditorAssetRegistry
  {
    std::vector<sc_world::AssetRegistryEntry> entries;
    bool loadFromFile(const char* path);
    const sc_world::AssetRegistryEntry* findByIds(sc_world::AssetId meshId, sc_world::AssetId materialId) const;
  };

  struct EditorDocument
  {
    sc_world::SectorCoord sector{};
    float sectorSize = 64.0f;
    float gridSize = 1.0f;
    bool snapToGrid = true;

    std::vector<EditorEntity> entities;
    uint64_t nextId = 1;
    uint64_t selectedId = 0;
  };

  struct EditorCamera
  {
    float position[3] = { 0.0f, 3.0f, 8.0f };
    float yaw = 3.14159f;
    float pitch = -0.15f;
    float fovDeg = 60.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    float moveSpeed = 10.0f;
    float lookSpeed = 0.003f;
  };

  struct Ray
  {
    float origin[3]{};
    float dir[3]{};
  };

  struct GizmoState
  {
    bool active = false;
    int axis = -1;
    float startAxisT = 0.0f;
    EditorTransform startTransform{};
    uint64_t entityId = 0;
  };

  struct Command
  {
    virtual ~Command() {}
    virtual void apply(EditorDocument* doc) = 0;
    virtual void undo(EditorDocument* doc) = 0;
  };

  struct CommandStack
  {
    std::vector<std::unique_ptr<Command>> undo;
    std::vector<std::unique_ptr<Command>> redo;
    void execute(std::unique_ptr<Command> cmd, EditorDocument* doc);
    void undoLast(EditorDocument* doc);
    void redoLast(EditorDocument* doc);
  };

  struct CmdPlaceEntity : public Command
  {
    EditorEntity entity{};
    void apply(EditorDocument* doc) override;
    void undo(EditorDocument* doc) override;
  };

  struct CmdDeleteEntity : public Command
  {
    EditorEntity entity{};
    void apply(EditorDocument* doc) override;
    void undo(EditorDocument* doc) override;
  };

  struct CmdTransformEntity : public Command
  {
    uint64_t entityId = 0;
    EditorTransform before{};
    EditorTransform after{};
    void apply(EditorDocument* doc) override;
    void undo(EditorDocument* doc) override;
  };

  struct CmdSetProperty : public Command
  {
    enum PropertyType
    {
      MeshId,
      MaterialId,
      Name,
      Tags
    };

    PropertyType type = MeshId;
    uint64_t entityId = 0;
    sc_world::AssetId oldId = 0;
    sc_world::AssetId newId = 0;
    char oldName[64] = "";
    char newName[64] = "";
    uint32_t oldTags = 0;
    uint32_t newTags = 0;

    void apply(EditorDocument* doc) override;
    void undo(EditorDocument* doc) override;
  };

  void InitDocument(EditorDocument* doc);
  EditorEntity* AddEntity(EditorDocument* doc, const sc_world::AssetRegistryEntry& asset, const EditorTransform& t);
  bool RemoveEntity(EditorDocument* doc, uint64_t id, EditorEntity* out_removed);
  EditorEntity* FindEntity(EditorDocument* doc, uint64_t id);
  bool SetTransform(EditorDocument* doc, uint64_t id, const EditorTransform& t);
  void SetSelected(EditorDocument* doc, uint64_t id);
  void ValidateSelection(EditorDocument* doc);

  void ResolveEntityAssets(EditorEntity* e,
                           ScRenderContext* render,
                           const EditorAssetRegistry& assets,
                           const AssetDatabase* assetDb,
                           EditorTextureCache* textureCache,
                           EditorModelCache* modelCache);

  void BuildDrawItems(const EditorDocument* doc, std::vector<ScRenderDrawItem>* out_items);
  void BuildDebugLines(const EditorDocument* doc, std::vector<ScRenderLine>* out_lines);

  sc::Mat4 CameraView(const EditorCamera* cam);
  sc::Mat4 CameraProj(const EditorCamera* cam, float aspect);
  void CameraForward(const EditorCamera* cam, float out_forward[3]);

  Ray computePickRay(const EditorCamera* cam,
                     float mouse_x,
                     float mouse_y,
                     float viewport_x,
                     float viewport_y,
                     float viewport_w,
                     float viewport_h);
  bool intersectRayAABB(const Ray& ray, const float bmin[3], const float bmax[3], float* out_t);

  Ray BuildPickRay(const EditorCamera* cam,
                   float mouse_x,
                   float mouse_y,
                   float viewport_x,
                   float viewport_y,
                   float viewport_w,
                   float viewport_h);
  uint64_t PickEntity(const EditorDocument* doc, const Ray& ray);

  bool GizmoTranslate(GizmoState* state,
                      EditorEntity* entity,
                      const EditorCamera* cam,
                      float mouse_x,
                      float mouse_y,
                      float viewport_x,
                      float viewport_y,
                      float viewport_w,
                      float viewport_h,
                      bool mouse_down,
                      bool mouse_dragging,
                      bool mouse_released,
                      bool snap_to_grid,
                      float grid_size);

  void SnapTransform(EditorTransform* t, float grid);

  void DocumentFromSectorFile(EditorDocument* doc,
                              const sc_world::SectorFile& file,
                              ScRenderContext* render,
                              const EditorAssetRegistry& assets,
                              const AssetDatabase* assetDb,
                              EditorTextureCache* textureCache,
                              EditorModelCache* modelCache);
  void SectorFileFromDocument(const EditorDocument* doc, sc_world::SectorFile* out_file);
}
}
