#pragma once
#include <string>
#include <vector>

#include "world_format.h"

namespace sc_world
{
  struct AssetRegistryEntry
  {
    std::string label;
    std::string mesh_path;
    std::string material_path;
    AssetId mesh_id = 0;
    AssetId material_id = 0;
  };

  bool LoadAssetRegistry(const char* path, std::vector<AssetRegistryEntry>& out_entries);
  const AssetRegistryEntry* FindByIds(const std::vector<AssetRegistryEntry>& entries,
                                      AssetId mesh_id,
                                      AssetId material_id);
  const AssetRegistryEntry* FindByMeshId(const std::vector<AssetRegistryEntry>& entries, AssetId mesh_id);
  const AssetRegistryEntry* FindByMaterialId(const std::vector<AssetRegistryEntry>& entries, AssetId material_id);
}
