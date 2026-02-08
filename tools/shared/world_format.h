#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sc_world
{
  using AssetId = uint64_t;

  static constexpr uint32_t kWorldMagic = 0x444C5257;  // "WRLD"
  static constexpr uint32_t kSectorMagic = 0x54434553; // "SECT"
  static constexpr uint32_t kWorldVersion = 1;
  static constexpr uint32_t kSectorVersion = 4;
  static constexpr uint32_t kInstanceNameMax = 64;
  static constexpr uint32_t kMaterialFlagUseTexture = 1u;

  struct SectorCoord
  {
    int32_t x = 0;
    int32_t z = 0;

    bool operator==(const SectorCoord& o) const { return x == o.x && z == o.z; }
    bool operator!=(const SectorCoord& o) const { return !(*this == o); }
  };

  struct Transform
  {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
  };

  struct Instance
  {
    uint64_t id = 0;
    AssetId model_id = 0;
    AssetId mesh_id = 0;
    AssetId material_id = 0;
    AssetId albedo_texture_id = 0;
    uint32_t material_flags = 0; // bit 0 = use texture override
    Transform transform{};
    char name[kInstanceNameMax] = "";
    uint32_t tags = 0;
  };

  struct LanePoint
  {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
  };

  struct Lane
  {
    uint64_t id = 0;
    uint32_t flags = 0;
    std::vector<LanePoint> points;
  };

  struct Spawner
  {
    uint64_t id = 0;
    Transform transform{};
    uint32_t type = 0;
    float rate = 1.0f;
  };

  struct Collider
  {
    uint64_t id = 0;
    uint32_t shape = 0; // 0=box, 1=sphere, 2=capsule
    Transform transform{};
    float size[3] = { 1.0f, 1.0f, 1.0f };
  };

  struct SectorFile
  {
    uint32_t version = kSectorVersion;
    SectorCoord sector{};
    std::vector<Instance> instances;
    std::vector<Lane> lanes;
    std::vector<Spawner> spawners;
    std::vector<Collider> colliders;
  };

  struct WorldManifest
  {
    uint32_t version = kWorldVersion;
    std::vector<SectorCoord> sectors;
  };

  AssetId HashAssetPath(const char* path);
  std::string NormalizePathForId(const std::string& path);

  bool WriteSectorFile(const char* path, const SectorFile& file);
  bool ReadSectorFile(const char* path, SectorFile* out_file);

  bool WriteWorldManifest(const char* path, const WorldManifest& manifest);
  bool ReadWorldManifest(const char* path, WorldManifest* out_manifest);

  std::string BuildSectorPath(const char* world_root, SectorCoord coord);
  std::string BuildWorldManifestPath(const char* world_root);
}
