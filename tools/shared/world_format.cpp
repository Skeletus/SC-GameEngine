#include "world_format.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace sc_world
{
  namespace
  {
    struct ChunkHeader
    {
      uint32_t id = 0;
      uint32_t size = 0;
    };

    static uint32_t MakeFourCC(char a, char b, char c, char d)
    {
      return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
    }

    template<typename T>
    static void WriteValue(std::ofstream& out, const T& value)
    {
      out.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    }

    template<typename T>
    static void ReadValue(std::ifstream& in, T& out_value)
    {
      in.read(reinterpret_cast<char*>(&out_value), static_cast<std::streamsize>(sizeof(T)));
    }

    static void WriteChunkHeader(std::ofstream& out, uint32_t id, uint32_t size)
    {
      ChunkHeader ch{};
      ch.id = id;
      ch.size = size;
      WriteValue(out, ch);
    }

    static bool ReadChunkHeader(std::ifstream& in, ChunkHeader& out_header)
    {
      ReadValue(in, out_header);
      return in.good();
    }
  }

  std::string NormalizePathForId(const std::string& path)
  {
    std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    std::string text = normalized.generic_string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
    {
      return static_cast<char>(std::tolower(c));
    });
    return text;
  }

  AssetId HashAssetPath(const char* path)
  {
    if (!path)
      return 0;
    const std::string normalized = NormalizePathForId(path);
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : normalized)
    {
      hash ^= static_cast<uint64_t>(ch);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  bool WriteSectorFile(const char* path, const SectorFile& file)
  {
    if (!path)
      return false;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
      return false;

    WriteValue(out, kSectorMagic);
    WriteValue(out, file.version);
    WriteValue(out, file.sector);

    const uint32_t kInstId = MakeFourCC('I', 'N', 'S', 'T');
    const uint32_t kLaneId = MakeFourCC('L', 'A', 'N', 'E');
    const uint32_t kSpwnId = MakeFourCC('S', 'P', 'W', 'N');
    const uint32_t kCollId = MakeFourCC('C', 'O', 'L', 'L');

    if (!file.instances.empty())
    {
      const uint32_t count = static_cast<uint32_t>(file.instances.size());
      const uint32_t baseRecordSize = sizeof(uint64_t) + sizeof(AssetId) + sizeof(AssetId) + sizeof(Transform) + sizeof(uint32_t);
      const uint32_t overrideSize = sizeof(AssetId) + sizeof(uint32_t);
      const bool writeName = (file.version >= 2);
      const bool writeOverrides = (file.version >= 3);
      const uint32_t recordSize = baseRecordSize + (writeName ? kInstanceNameMax : 0u) + (writeOverrides ? overrideSize : 0u);
      const uint32_t chunkSize = sizeof(uint32_t) + count * recordSize;
      WriteChunkHeader(out, kInstId, chunkSize);
      WriteValue(out, count);
      for (const Instance& inst : file.instances)
      {
        WriteValue(out, inst.id);
        WriteValue(out, inst.mesh_id);
        WriteValue(out, inst.material_id);
        WriteValue(out, inst.transform);
        if (writeName)
          out.write(inst.name, static_cast<std::streamsize>(kInstanceNameMax));
        WriteValue(out, inst.tags);
        if (writeOverrides)
        {
          WriteValue(out, inst.albedo_texture_id);
          WriteValue(out, inst.material_flags);
        }
      }
    }

    if (!file.lanes.empty())
    {
      uint32_t chunkSize = sizeof(uint32_t);
      for (const Lane& lane : file.lanes)
      {
        chunkSize += sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t);
        chunkSize += static_cast<uint32_t>(lane.points.size() * sizeof(LanePoint));
      }

      WriteChunkHeader(out, kLaneId, chunkSize);
      const uint32_t count = static_cast<uint32_t>(file.lanes.size());
      WriteValue(out, count);
      for (const Lane& lane : file.lanes)
      {
        WriteValue(out, lane.id);
        WriteValue(out, lane.flags);
        const uint32_t pcount = static_cast<uint32_t>(lane.points.size());
        WriteValue(out, pcount);
        for (const LanePoint& pt : lane.points)
          WriteValue(out, pt);
      }
    }

    if (!file.spawners.empty())
    {
      const uint32_t count = static_cast<uint32_t>(file.spawners.size());
      const uint32_t recordSize = sizeof(uint64_t) + sizeof(Transform) + sizeof(uint32_t) + sizeof(float);
      const uint32_t chunkSize = sizeof(uint32_t) + count * recordSize;
      WriteChunkHeader(out, kSpwnId, chunkSize);
      WriteValue(out, count);
      for (const Spawner& sp : file.spawners)
      {
        WriteValue(out, sp.id);
        WriteValue(out, sp.transform);
        WriteValue(out, sp.type);
        WriteValue(out, sp.rate);
      }
    }

    if (!file.colliders.empty())
    {
      const uint32_t count = static_cast<uint32_t>(file.colliders.size());
      const uint32_t recordSize = sizeof(uint64_t) + sizeof(uint32_t) + sizeof(Transform) + sizeof(float) * 3;
      const uint32_t chunkSize = sizeof(uint32_t) + count * recordSize;
      WriteChunkHeader(out, kCollId, chunkSize);
      WriteValue(out, count);
      for (const Collider& col : file.colliders)
      {
        WriteValue(out, col.id);
        WriteValue(out, col.shape);
        WriteValue(out, col.transform);
        out.write(reinterpret_cast<const char*>(col.size), static_cast<std::streamsize>(sizeof(float) * 3));
      }
    }

    return out.good();
  }

  bool ReadSectorFile(const char* path, SectorFile* out_file)
  {
    if (!path || !out_file)
      return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
      return false;

    uint32_t magic = 0;
    ReadValue(in, magic);
    if (magic != kSectorMagic)
      return false;

    ReadValue(in, out_file->version);
    ReadValue(in, out_file->sector);

    out_file->instances.clear();
    out_file->lanes.clear();
    out_file->spawners.clear();
    out_file->colliders.clear();

    const uint32_t kInstId = MakeFourCC('I', 'N', 'S', 'T');
    const uint32_t kLaneId = MakeFourCC('L', 'A', 'N', 'E');
    const uint32_t kSpwnId = MakeFourCC('S', 'P', 'W', 'N');
    const uint32_t kCollId = MakeFourCC('C', 'O', 'L', 'L');

    while (in.good() && !in.eof())
    {
      ChunkHeader ch{};
      if (!ReadChunkHeader(in, ch))
        break;

      if (ch.size == 0)
        continue;

      if (ch.id == kInstId)
      {
        uint32_t count = 0;
        ReadValue(in, count);
        out_file->instances.resize(count);

        const uint32_t baseRecordSize = sizeof(uint64_t) + sizeof(AssetId) + sizeof(AssetId) + sizeof(Transform) + sizeof(uint32_t);
        const uint32_t overrideSize = sizeof(AssetId) + sizeof(uint32_t);
        uint32_t recordSize = baseRecordSize;
        if (count > 0 && ch.size >= sizeof(uint32_t))
          recordSize = (ch.size - sizeof(uint32_t)) / count;

        const uint32_t nameRecordSize = baseRecordSize + kInstanceNameMax;
        const bool hasName = (recordSize >= nameRecordSize);
        const uint32_t baseWithName = hasName ? nameRecordSize : baseRecordSize;
        const bool hasOverrides = (recordSize >= baseWithName + overrideSize);
        const uint32_t expectedSize = baseWithName + (hasOverrides ? overrideSize : 0u);

        for (uint32_t i = 0; i < count; ++i)
        {
          Instance inst{};
          ReadValue(in, inst.id);
          ReadValue(in, inst.mesh_id);
          ReadValue(in, inst.material_id);
          ReadValue(in, inst.transform);
          if (hasName)
          {
            in.read(inst.name, static_cast<std::streamsize>(kInstanceNameMax));
            inst.name[kInstanceNameMax - 1] = '\0';
          }
          else
          {
            inst.name[0] = '\0';
          }
          ReadValue(in, inst.tags);
          if (hasOverrides)
          {
            ReadValue(in, inst.albedo_texture_id);
            ReadValue(in, inst.material_flags);
          }
          else
          {
            inst.albedo_texture_id = 0;
            inst.material_flags = 0;
          }

          if (recordSize > expectedSize)
          {
            const uint32_t remaining = recordSize - expectedSize;
            in.seekg(static_cast<std::streamoff>(remaining), std::ios::cur);
          }

          out_file->instances[i] = inst;
        }
      }
      else if (ch.id == kLaneId)
      {
        uint32_t count = 0;
        ReadValue(in, count);
        out_file->lanes.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
          Lane lane{};
          uint32_t pcount = 0;
          ReadValue(in, lane.id);
          ReadValue(in, lane.flags);
          ReadValue(in, pcount);
          lane.points.resize(pcount);
          for (uint32_t p = 0; p < pcount; ++p)
            ReadValue(in, lane.points[p]);
          out_file->lanes[i] = lane;
        }
      }
      else if (ch.id == kSpwnId)
      {
        uint32_t count = 0;
        ReadValue(in, count);
        out_file->spawners.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
          Spawner sp{};
          ReadValue(in, sp.id);
          ReadValue(in, sp.transform);
          ReadValue(in, sp.type);
          ReadValue(in, sp.rate);
          out_file->spawners[i] = sp;
        }
      }
      else if (ch.id == kCollId)
      {
        uint32_t count = 0;
        ReadValue(in, count);
        out_file->colliders.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
          Collider col{};
          ReadValue(in, col.id);
          ReadValue(in, col.shape);
          ReadValue(in, col.transform);
          in.read(reinterpret_cast<char*>(col.size), static_cast<std::streamsize>(sizeof(float) * 3));
          out_file->colliders[i] = col;
        }
      }
      else
      {
        in.seekg(static_cast<std::streamoff>(ch.size), std::ios::cur);
      }
    }

    return true;
  }

  bool WriteWorldManifest(const char* path, const WorldManifest& manifest)
  {
    if (!path)
      return false;

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
      return false;

    WriteValue(out, kWorldMagic);
    WriteValue(out, manifest.version);
    const uint32_t count = static_cast<uint32_t>(manifest.sectors.size());
    WriteValue(out, count);
    for (const SectorCoord& coord : manifest.sectors)
      WriteValue(out, coord);
    return out.good();
  }

  bool ReadWorldManifest(const char* path, WorldManifest* out_manifest)
  {
    if (!path || !out_manifest)
      return false;

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
      return false;

    uint32_t magic = 0;
    ReadValue(in, magic);
    if (magic != kWorldMagic)
      return false;

    ReadValue(in, out_manifest->version);
    uint32_t count = 0;
    ReadValue(in, count);
    out_manifest->sectors.resize(count);
    for (uint32_t i = 0; i < count; ++i)
      ReadValue(in, out_manifest->sectors[i]);

    return true;
  }

  std::string BuildSectorPath(const char* world_root, SectorCoord coord)
  {
    std::filesystem::path p(world_root ? world_root : ".");
    p /= "sectors";
    const std::string name = "sector_" + std::to_string(coord.x) + "_" + std::to_string(coord.z) + ".scsector";
    p /= name;
    return p.string();
  }

  std::string BuildWorldManifestPath(const char* world_root)
  {
    std::filesystem::path p(world_root ? world_root : ".");
    p /= "world_manifest.scworld";
    return p.string();
  }
}
