#include "asset_registry.h"

#include <fstream>
#include <sstream>

namespace sc_world
{
  bool LoadAssetRegistry(const char* path, std::vector<AssetRegistryEntry>& out_entries)
  {
    out_entries.clear();
    if (!path)
      return false;

    std::ifstream in(path);
    if (!in.is_open())
      return false;

    std::string line;
    while (std::getline(in, line))
    {
      if (line.empty())
        continue;
      if (line[0] == '#')
        continue;

      std::stringstream ss(line);
      std::string label;
      std::string mesh;
      std::string material;

      if (!std::getline(ss, label, '|'))
        continue;
      if (!std::getline(ss, mesh, '|'))
        continue;
      if (!std::getline(ss, material, '|'))
        continue;

      AssetRegistryEntry entry{};
      entry.label = label;
      entry.mesh_path = mesh;
      entry.material_path = material;
      entry.mesh_id = HashAssetPath(mesh.c_str());
      entry.material_id = HashAssetPath(material.c_str());
      out_entries.push_back(std::move(entry));
    }

    return !out_entries.empty();
  }

  const AssetRegistryEntry* FindByIds(const std::vector<AssetRegistryEntry>& entries,
                                      AssetId mesh_id,
                                      AssetId material_id)
  {
    for (const auto& entry : entries)
    {
      if (entry.mesh_id == mesh_id && entry.material_id == material_id)
        return &entry;
    }
    return nullptr;
  }

  const AssetRegistryEntry* FindByMeshId(const std::vector<AssetRegistryEntry>& entries, AssetId mesh_id)
  {
    for (const auto& entry : entries)
    {
      if (entry.mesh_id == mesh_id)
        return &entry;
    }
    return nullptr;
  }

  const AssetRegistryEntry* FindByMaterialId(const std::vector<AssetRegistryEntry>& entries, AssetId material_id)
  {
    for (const auto& entry : entries)
    {
      if (entry.material_id == material_id)
        return &entry;
    }
    return nullptr;
  }
}
