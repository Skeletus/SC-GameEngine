#include "sc_world_partition.h"

#include "sc_jobs.h"
#include "sc_assets.h"
#include "sc_paths.h"
#include "world_format.h"
#include "sc_physics.h"
#include "sc_traffic_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

namespace sc
{
  namespace
  {
    static constexpr float kPi = 3.1415926535f;
    static constexpr AABB kUnitCubeBounds{ { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
    static constexpr const char* kMeshCubePath = "meshes/cube";
    static constexpr const char* kMeshTrianglePath = "meshes/triangle";
    static constexpr const char* kMaterialUnlitPath = "materials/unlit";
    static constexpr const char* kMaterialCheckerPath = "materials/checker";
    static constexpr const char* kMaterialTestPath = "materials/test";

    static uint32_t mix32(uint32_t x)
    {
      x ^= x >> 16;
      x *= 0x7feb352du;
      x ^= x >> 15;
      x *= 0x846ca68bu;
      x ^= x >> 16;
      return x;
    }

    static uint32_t hashCoordSeed(uint32_t seed, const SectorCoord& coord)
    {
      uint32_t h = seed;
      h ^= mix32(static_cast<uint32_t>(coord.x) * 73856093u);
      h ^= mix32(static_cast<uint32_t>(coord.z) * 19349663u);
      h = mix32(h + 0x9e3779b9u);
      return h;
    }

    static float rand01(uint32_t& state)
    {
      state = mix32(state + 0x6d2b79f5u);
      return static_cast<float>(state & 0x00FFFFFFu) / 16777215.0f;
    }

    static float lerp(float a, float b, float t)
    {
      return a + (b - a) * t;
    }

    static AssetId assetIdFromPath(const char* path)
    {
      if (!path)
        return 0;
      return sc::fnv1a64(sc::normalizePathForId(path));
    }

    static std::filesystem::path worldRootPath()
    {
      if (const char* envRoot = std::getenv("SC_WORLD_ROOT"))
      {
        if (*envRoot != '\0')
          return std::filesystem::path(envRoot);
      }
      return sc::resolveAssetPath("world");
    }

    static bool coordLess(const SectorCoord& a, const SectorCoord& b)
    {
      if (a.z != b.z) return a.z < b.z;
      return a.x < b.x;
    }

    static int sectorDistanceSq(const SectorCoord& a, const SectorCoord& b)
    {
      const int dx = a.x - b.x;
      const int dz = a.z - b.z;
      return dx * dx + dz * dz;
    }

    static float normalize2d(float& x, float& z)
    {
      const float lenSq = x * x + z * z;
      if (lenSq <= 1e-6f)
        return 0.0f;
      const float inv = 1.0f / std::sqrt(lenSq);
      x *= inv;
      z *= inv;
      return std::sqrt(lenSq);
    }

    static void generateSectorSpawnsStatic(const WorldPartitionConfig& config,
                                           const SectorCoord& coord,
                                           std::vector<SpawnRecord>& outSpawns)
    {
      const float size = config.sectorSizeMeters;
      const float minX = static_cast<float>(coord.x) * size;
      const float minZ = static_cast<float>(coord.z) * size;
      const float centerX = minX + size * 0.5f;
      const float centerZ = minZ + size * 0.5f;

      uint32_t rng = hashCoordSeed(config.seed, coord);
      const uint32_t countRange = config.propsPerSectorMax - config.propsPerSectorMin + 1u;
      const uint32_t propCount = config.propsPerSectorMin + (countRange > 0 ? (mix32(rng) % countRange) : 0u);

      outSpawns.clear();
      outSpawns.reserve(propCount + (config.includeGroundPlane ? 1u : 0u));

      if (config.includeGroundPlane)
      {
        SpawnRecord ground{};
        std::snprintf(ground.name, Name::kMax, "Ground_%d_%d", coord.x, coord.z);
        ground.position[0] = centerX;
        ground.position[1] = -0.55f;
        ground.position[2] = centerZ;
        ground.scale[0] = size;
        ground.scale[1] = 0.10f;
        ground.scale[2] = size;
        ground.meshAssetId = assetIdFromPath(kMeshCubePath);
        ground.materialAssetId = assetIdFromPath(kMaterialUnlitPath);
        ground.localBounds = kUnitCubeBounds;
        outSpawns.push_back(ground);
      }

      const float pad = 1.0f;
      for (uint32_t i = 0; i < propCount; ++i)
      {
        SpawnRecord rec{};
        std::snprintf(rec.name, Name::kMax, "Prop_%d_%d_%u", coord.x, coord.z, i);

        const float x = lerp(minX + pad, minX + size - pad, rand01(rng));
        const float z = lerp(minZ + pad, minZ + size - pad, rand01(rng));

        const float sx = lerp(0.4f, 1.9f, rand01(rng));
        const float sy = lerp(0.5f, 3.2f, rand01(rng));
        const float sz = lerp(0.4f, 1.9f, rand01(rng));

        rec.position[0] = x;
        rec.position[1] = sy * 0.5f;
        rec.position[2] = z;
        rec.rotation[1] = rand01(rng) * (kPi * 2.0f);
        rec.scale[0] = sx;
        rec.scale[1] = sy;
        rec.scale[2] = sz;

        const float m = rand01(rng);
        rec.materialAssetId = (m < 0.40f) ? assetIdFromPath(kMaterialCheckerPath)
                                         : ((m < 0.80f) ? assetIdFromPath(kMaterialTestPath)
                                                        : assetIdFromPath(kMaterialUnlitPath));
        rec.meshAssetId = (rand01(rng) < 0.90f) ? assetIdFromPath(kMeshCubePath)
                                                : assetIdFromPath(kMeshTrianglePath);
        rec.localBounds = kUnitCubeBounds;

        outSpawns.push_back(rec);
      }
    }

    static Entity pickActiveCamera(World& world, Transform*& outTransform)
    {
      Entity active = kInvalidEntity;
      Entity fallback = kInvalidEntity;
      Transform* activeTransform = nullptr;
      Transform* fallbackTransform = nullptr;

      world.ForEach<Camera, Transform>([&](Entity e, Camera& cam, Transform& tr)
      {
        if (!activeTransform && cam.active)
        {
          active = e;
          activeTransform = &tr;
        }
        if (!fallbackTransform)
        {
          fallback = e;
          fallbackTransform = &tr;
        }
      });

      if (activeTransform)
      {
        outTransform = activeTransform;
        return active;
      }
      outTransform = fallbackTransform;
      return fallback;
    }

    static void pushDrawItem(RenderFrameData& frame, Entity e, const Transform& t, const RenderMesh& rm)
    {
      DrawItem cmd{};
      cmd.entity = e;
      cmd.meshId = rm.meshId;
      cmd.materialId = rm.materialId;
      cmd.model = t.worldMatrix;
      frame.draws.push_back(cmd);
    }
  }

  size_t SectorCoordHash::operator()(const SectorCoord& c) const noexcept
  {
    const uint64_t x = static_cast<uint32_t>(c.x);
    const uint64_t z = static_cast<uint32_t>(c.z);
    return static_cast<size_t>((x * 73856093ull) ^ (z * 19349663ull));
  }

  void WorldPartition::configure(const WorldPartitionConfig& config)
  {
    m_config = config;
    if (m_config.sectorSizeMeters <= 0.001f)
      m_config.sectorSizeMeters = 64.0f;
    if (m_config.propsPerSectorMax < m_config.propsPerSectorMin)
      m_config.propsPerSectorMax = m_config.propsPerSectorMin;
    if (m_sectors.empty())
      m_sectors.reserve(256);
  }

  void WorldPartition::setAssetRegistryPath(const char* path)
  {
    if (!path || path[0] == '\0')
      return;
    m_assetRegistryPath = path;
    m_assetRegistryLoaded = false;
  }

  void WorldPartition::setPinnedCenters(const std::vector<SectorCoord>& centers, uint32_t radius)
  {
    m_pinnedCenters = centers;
    m_pinnedRadius = radius;
    m_pinnedExpanded.clear();

    const int32_t r = static_cast<int32_t>(radius);
    for (const SectorCoord& c : m_pinnedCenters)
    {
      for (int32_t dz = -r; dz <= r; ++dz)
      {
        for (int32_t dx = -r; dx <= r; ++dx)
        {
          const SectorCoord coord{ c.x + dx, c.z + dz };
          bool exists = false;
          for (const SectorCoord& e : m_pinnedExpanded)
          {
            if (e == coord)
            {
              exists = true;
              break;
            }
          }
          if (!exists)
            m_pinnedExpanded.push_back(coord);
        }
      }
    }
  }

  SectorCoord WorldPartition::worldToSector(const Vec3& pos) const
  {
    const float inv = 1.0f / m_config.sectorSizeMeters;
    SectorCoord c{};
    c.x = static_cast<int32_t>(std::floor(pos.x * inv));
    c.z = static_cast<int32_t>(std::floor(pos.z * inv));
    return c;
  }

  AABB WorldPartition::sectorBounds(const SectorCoord& coord) const
  {
    const float size = m_config.sectorSizeMeters;
    const float minX = static_cast<float>(coord.x) * size;
    const float minZ = static_cast<float>(coord.z) * size;

    AABB box{};
    box.min = { minX, -1.0f, minZ };
    box.max = { minX + size, 8.0f, minZ + size };
    return box;
  }

  Sector* WorldPartition::findSector(const SectorCoord& coord)
  {
    auto it = m_sectors.find(coord);
    if (it == m_sectors.end())
      return nullptr;
    return &it->second;
  }

  const Sector* WorldPartition::findSector(const SectorCoord& coord) const
  {
    auto it = m_sectors.find(coord);
    if (it == m_sectors.end())
      return nullptr;
    return &it->second;
  }

  Sector& WorldPartition::getOrCreateSector(const SectorCoord& coord)
  {
    auto it = m_sectors.find(coord);
    if (it != m_sectors.end())
      return it->second;

    Sector sector{};
    sector.coord = coord;
    auto [insertedIt, _] = m_sectors.emplace(coord, std::move(sector));
    return insertedIt->second;
  }

  void WorldPartition::generateSectorSpawns(Sector& sector)
  {
    generateSectorSpawnsStatic(m_config, sector.coord, sector.spawns);
  }

  void WorldPartition::markQueued(Sector& sector)
  {
    if (sector.state == SectorLoadState::Queued ||
        sector.state == SectorLoadState::Loading ||
        sector.state == SectorLoadState::ReadyToActivate ||
        sector.state == SectorLoadState::Active)
    {
      sector.lastTouchedFrame = m_frameCounter;
      return;
    }

    sector.state = SectorLoadState::Queued;
    sector.lastTouchedFrame = m_frameCounter;
    sector.queuedAt = nowTicks();
    sector.requestId = m_requestIdCounter++;
    sector.pendingDespawns = 0;
  }

  void WorldPartition::markReady(Sector& sector)
  {
    sector.state = SectorLoadState::ReadyToActivate;
    sector.lastTouchedFrame = m_frameCounter;
  }

  void WorldPartition::markActive(Sector& sector, uint32_t spawnCount)
  {
    if (sector.state == SectorLoadState::Active)
    {
      sector.lastTouchedFrame = m_frameCounter;
      return;
    }

    sector.state = SectorLoadState::Active;
    sector.lastTouchedFrame = m_frameCounter;
    sector.activateAt = nowTicks();
    sector.pendingDespawns = 0;
    m_activeSectorCount++;
    m_activeEntityEstimate += spawnCount;

    auto unloadedIt = std::find(m_unloadedThisFrame.begin(), m_unloadedThisFrame.end(), sector.coord);
    if (unloadedIt != m_unloadedThisFrame.end())
      m_unloadedThisFrame.erase(unloadedIt);
    m_loadedThisFrame.push_back(sector.coord);
  }

  void WorldPartition::markUnloading(Sector& sector)
  {
    if (sector.state == SectorLoadState::Unloading)
      return;

    if (sector.state == SectorLoadState::Active && m_activeSectorCount > 0)
      m_activeSectorCount--;

    sector.state = SectorLoadState::Unloading;
    sector.lastTouchedFrame = m_frameCounter;
  }

  void WorldPartition::markUnloaded(Sector& sector)
  {
    if (sector.state == SectorLoadState::Active && m_activeSectorCount > 0)
      m_activeSectorCount--;

    sector.state = SectorLoadState::Unloaded;
    sector.lastTouchedFrame = m_frameCounter;
    sector.spawns.clear();
    sector.entities.clear();
    sector.trafficEntities.clear();
    sector.trafficSpawned = false;
    sector.pendingDespawns = 0;

    auto loadedIt = std::find(m_loadedThisFrame.begin(), m_loadedThisFrame.end(), sector.coord);
    if (loadedIt != m_loadedThisFrame.end())
      m_loadedThisFrame.erase(loadedIt);
    m_unloadedThisFrame.push_back(sector.coord);
  }

  Sector& WorldPartition::ensureSectorLoaded(const SectorCoord& coord)
  {
    Sector& sector = getOrCreateSector(coord);
    if (sector.state != SectorLoadState::Active)
    {
      if (sector.spawns.empty())
        generateSectorSpawns(sector);
      markActive(sector, static_cast<uint32_t>(sector.spawns.size()));
    }
    return sector;
  }

  bool WorldPartition::unloadSector(const SectorCoord& coord)
  {
    Sector* sector = findSector(coord);
    if (!sector)
      return false;
    if (sector->state != SectorLoadState::Active)
      return false;
    markUnloaded(*sector);
    return true;
  }

  void WorldPartition::clearFrameDeltas()
  {
    m_loadedThisFrame.clear();
    m_unloadedThisFrame.clear();
    m_frameStats.loadedThisFrame = 0;
    m_frameStats.unloadedThisFrame = 0;
    m_frameStats.entitiesSpawned = 0;
    m_frameStats.entitiesDespawned = 0;
    m_frameStats.rejectedBySectorBudget = 0;
    m_frameStats.rejectedByEntityBudget = 0;
    m_frameStats.completedLoads = 0;
    m_frameStats.activations = 0;
    m_frameStats.despawns = 0;
    m_frameStats.avgLoadMs = 0.0f;
    m_frameStats.maxLoadMs = 0.0f;
    m_frameStats.pumpLoadsMs = 0.0f;
    m_frameStats.pumpUnloadsMs = 0.0f;
  }

  void WorldPartition::updateActiveSet(const Vec3& cameraPos,
                                       const Vec3& cameraForward,
                                       uint32_t loadRadiusSectors,
                                       uint32_t unloadRadiusSectors,
                                       const WorldPartitionBudget& budget,
                                       bool useFrustumBias,
                                       float frustumBiasWeight,
                                       bool allowScheduling)
  {
    m_frameCounter++;
    clearFrameDeltas();

    if (unloadRadiusSectors <= loadRadiusSectors)
      unloadRadiusSectors = loadRadiusSectors + 1u;

    const SectorCoord cameraSector = worldToSector(cameraPos);
    m_lastCameraSector = cameraSector;
    m_lastCameraForward = cameraForward;
    m_lastLoadRadius = loadRadiusSectors;
    m_lastUnloadRadius = unloadRadiusSectors;
    m_lastUseFrustumBias = useFrustumBias;
    m_lastFrustumBiasWeight = frustumBiasWeight;

    m_frameStats.cameraSector = cameraSector;
    m_frameStats.loadRadiusSectors = loadRadiusSectors;
    m_frameStats.unloadRadiusSectors = unloadRadiusSectors;

    const int32_t r = static_cast<int32_t>(loadRadiusSectors);
    const uint32_t side = static_cast<uint32_t>(r * 2 + 1);

    m_scratchDesired.clear();
    m_scratchDesiredLookup.clear();
    m_scratchDesired.reserve(static_cast<size_t>(side) * static_cast<size_t>(side) + m_pinnedExpanded.size());

    auto pushDesired = [&](const SectorCoord& coord)
    {
      for (const SectorCoord& e : m_scratchDesiredLookup)
      {
        if (e == coord)
          return;
      }
      m_scratchDesiredLookup.push_back(coord);
      m_scratchDesired.push_back(coord);
    };

    for (int32_t dz = -r; dz <= r; ++dz)
    {
      for (int32_t dx = -r; dx <= r; ++dx)
      {
        pushDesired({ cameraSector.x + dx, cameraSector.z + dz });
      }
    }

    for (const SectorCoord& coord : m_pinnedExpanded)
      pushDesired(coord);

    m_frameStats.desiredSectors = static_cast<uint32_t>(m_scratchDesired.size());

    std::sort(m_scratchDesired.begin(), m_scratchDesired.end(),
      [&](const SectorCoord& a, const SectorCoord& b)
      {
        const float pa = sectorPriority(a, cameraSector, cameraForward, frustumBiasWeight, useFrustumBias);
        const float pb = sectorPriority(b, cameraSector, cameraForward, frustumBiasWeight, useFrustumBias);
        if (pa != pb) return pa < pb;
        return coordLess(a, b);
      });

    uint32_t reserved = 0;
    for (const auto& [coord, sector] : m_sectors)
    {
      (void)coord;
      if (sector.state == SectorLoadState::Queued ||
          sector.state == SectorLoadState::Loading ||
          sector.state == SectorLoadState::ReadyToActivate ||
          sector.state == SectorLoadState::Active)
      {
        reserved++;
      }
    }

    if (allowScheduling)
    {
      for (const SectorCoord& coord : m_scratchDesired)
      {
        Sector& sector = getOrCreateSector(coord);
        if (sector.state == SectorLoadState::Queued ||
            sector.state == SectorLoadState::Loading ||
            sector.state == SectorLoadState::ReadyToActivate ||
            sector.state == SectorLoadState::Active)
        {
          sector.lastTouchedFrame = m_frameCounter;
          continue;
        }

        if (sector.state == SectorLoadState::Unloading)
          continue;

        const bool pinned = isSectorPinned(coord);
        if (!pinned && budget.maxActiveSectors > 0 && reserved >= budget.maxActiveSectors)
        {
          m_frameStats.rejectedBySectorBudget++;
          continue;
        }

        markQueued(sector);
        m_pendingLoads.push_back(coord);
        reserved++;
      }
    }

    const int unloadRadiusSq = static_cast<int>(unloadRadiusSectors) * static_cast<int>(unloadRadiusSectors);

    if (allowScheduling)
    {
      for (auto& [coord, sector] : m_sectors)
      {
        if (isSectorPinned(coord))
        {
          if (sector.state == SectorLoadState::Active)
            sector.lastTouchedFrame = m_frameCounter;
          continue;
        }

        const int distSq = sectorDistanceSq(coord, cameraSector);
        if (distSq <= unloadRadiusSq)
        {
          if (sector.state == SectorLoadState::Active)
            sector.lastTouchedFrame = m_frameCounter;
          continue;
        }

        switch (sector.state)
        {
          case SectorLoadState::Active:
            markUnloading(sector);
            queueUnloadEntities(sector);
            break;
          case SectorLoadState::ReadyToActivate:
            markUnloaded(sector);
            break;
          case SectorLoadState::Queued:
          case SectorLoadState::Loading:
            sector.state = SectorLoadState::Unloaded;
            sector.requestId++;
            sector.spawns.clear();
            sector.entities.clear();
            sector.trafficEntities.clear();
            sector.trafficSpawned = false;
            sector.pendingDespawns = 0;
            break;
          default:
            break;
        }
      }
    }

    uint32_t queued = 0;
    uint32_t loading = 0;
    uint32_t ready = 0;
    uint32_t active = 0;
    uint32_t unloading = 0;
    for (const auto& [coord, sector] : m_sectors)
    {
      (void)coord;
      switch (sector.state)
      {
        case SectorLoadState::Queued: queued++; break;
        case SectorLoadState::Loading: loading++; break;
        case SectorLoadState::ReadyToActivate: ready++; break;
        case SectorLoadState::Active: active++; break;
        case SectorLoadState::Unloading: unloading++; break;
        default: break;
      }
    }

    m_frameStats.queuedSectors = queued;
    m_frameStats.loadingSectors = loading;
    m_frameStats.readySectors = ready;
    m_frameStats.activeSectors = active;
    m_frameStats.unloadingSectors = unloading;
    m_frameStats.loadedSectors = m_activeSectorCount;
    m_frameStats.loadedThisFrame = static_cast<uint32_t>(m_loadedThisFrame.size());
    m_frameStats.unloadedThisFrame = static_cast<uint32_t>(m_unloadedThisFrame.size());
    m_frameStats.estimatedSectorEntities = m_activeEntityEstimate;
  }

  float WorldPartition::sectorPriority(const SectorCoord& coord,
                                       const SectorCoord& cameraSector,
                                       const Vec3& cameraForward,
                                       float frustumBiasWeight,
                                       bool useFrustumBias) const
  {
    const int dx = coord.x - cameraSector.x;
    const int dz = coord.z - cameraSector.z;
    const float distSq = static_cast<float>(dx * dx + dz * dz);
    if (!useFrustumBias || frustumBiasWeight <= 0.0f)
      return distSq;

    float fx = cameraForward.x;
    float fz = cameraForward.z;
    if (normalize2d(fx, fz) <= 1e-6f)
      return distSq;

    float dirx = static_cast<float>(dx);
    float dirz = static_cast<float>(dz);
    if (normalize2d(dirx, dirz) <= 1e-6f)
      return distSq;

    const float dot = dirx * fx + dirz * fz;
    return distSq - dot * frustumBiasWeight;
  }

  bool WorldPartition::isSectorDesired(const SectorCoord& coord) const
  {
    if (isSectorPinned(coord))
      return true;
    const int distSq = sectorDistanceSq(coord, m_lastCameraSector);
    const int r = static_cast<int>(m_lastUnloadRadius);
    if (r <= 0)
      return distSq == 0;
    return distSq <= r * r;
  }

  bool WorldPartition::isSectorPinned(const SectorCoord& coord) const
  {
    for (const SectorCoord& c : m_pinnedExpanded)
    {
      if (c == coord)
        return true;
    }
    return false;
  }

  void WorldPartition::queueUnloadEntities(Sector& sector)
  {
    const uint32_t staticCount = static_cast<uint32_t>(sector.entities.size());
    const uint32_t trafficCount = static_cast<uint32_t>(sector.trafficEntities.size());
    const uint32_t total = staticCount + trafficCount;

    if (total == 0)
    {
      sector.pendingDespawns = 0;
      return;
    }

    for (const Entity e : sector.entities)
      m_pendingDespawns.push_back({ e, sector.coord });
    for (const Entity e : sector.trafficEntities)
      m_pendingDespawns.push_back({ e, sector.coord });

    sector.pendingDespawns = total;
    sector.entities.clear();
    sector.trafficEntities.clear();
    sector.trafficSpawned = false;
  }

  bool WorldPartition::readSectorFile(const SectorCoord& coord, std::vector<SpawnRecord>& outSpawns) const
  {
    sc_world::SectorFile file{};
    const std::filesystem::path root = worldRootPath();
    const sc_world::SectorCoord sc{ coord.x, coord.z };
    const std::string path = sc_world::BuildSectorPath(root.string().c_str(), sc);
    if (!sc_world::ReadSectorFile(path.c_str(), &file))
      return false;

    outSpawns.clear();
    outSpawns.reserve(file.instances.size());
    for (const sc_world::Instance& inst : file.instances)
    {
      SpawnRecord rec{};
      if (inst.name[0] != '\0')
        std::snprintf(rec.name, Name::kMax, "%s", inst.name);
      else
        std::snprintf(rec.name, Name::kMax, "Inst_%llu", (unsigned long long)inst.id);
      rec.position[0] = inst.transform.position[0];
      rec.position[1] = inst.transform.position[1];
      rec.position[2] = inst.transform.position[2];
      rec.rotation[0] = inst.transform.rotation[0];
      rec.rotation[1] = inst.transform.rotation[1];
      rec.rotation[2] = inst.transform.rotation[2];
      rec.scale[0] = inst.transform.scale[0];
      rec.scale[1] = inst.transform.scale[1];
      rec.scale[2] = inst.transform.scale[2];
      rec.meshAssetId = inst.mesh_id;
      rec.materialAssetId = inst.material_id;
      rec.localBounds = kUnitCubeBounds;
      outSpawns.push_back(rec);
    }

    return true;
  }

  void WorldPartition::ensureAssetRegistryLoaded()
  {
    if (m_assetRegistryLoaded)
      return;
    m_assetRegistryLoaded = true;
    m_assetRegistry.clear();

    if (m_assetRegistryPath.empty())
      return;

    const std::filesystem::path resolved = sc::resolveAssetPath(m_assetRegistryPath);
    sc_world::LoadAssetRegistry(resolved.string().c_str(), m_assetRegistry);
  }

  uint32_t WorldPartition::resolveMeshHandle(AssetId assetId)
  {
    if (!m_assets || assetId == 0)
      return 0;

    auto it = m_meshHandleCache.find(assetId);
    if (it != m_meshHandleCache.end())
      return it->second;

    ensureAssetRegistryLoaded();
    const sc_world::AssetRegistryEntry* entry = sc_world::FindByMeshId(m_assetRegistry, assetId);
    const char* meshPath = entry ? entry->mesh_path.c_str() : kMeshCubePath;
    const MeshHandle handle = m_assets->loadMesh(meshPath);
    const uint32_t resolved = (handle == kInvalidMeshHandle) ? 0u : handle;
    m_meshHandleCache[assetId] = resolved;
    return resolved;
  }

  uint32_t WorldPartition::resolveMaterialHandle(AssetId assetId)
  {
    if (!m_assets || assetId == 0)
      return 0;

    auto it = m_materialHandleCache.find(assetId);
    if (it != m_materialHandleCache.end())
      return it->second;

    ensureAssetRegistryLoaded();
    const sc_world::AssetRegistryEntry* entry = sc_world::FindByMaterialId(m_assetRegistry, assetId);
    std::string path = entry ? entry->material_path : std::string(kMaterialUnlitPath);

    sc::MaterialDesc desc{};
    if (path.find("unlit") != std::string::npos)
    {
      desc.unlit = true;
    }
    else
    {
      if (path == kMaterialCheckerPath)
        path = "textures/checker.ppm";
      else if (path == kMaterialTestPath)
        path = "textures/test.ppm";

      desc.albedo = m_assets->loadTexture2D(path);
      desc.unlit = false;
    }

    const MaterialHandle handle = m_assets->createMaterial(desc);
    const uint32_t resolved = (handle == kInvalidMaterialHandle) ? 0u : handle;
    m_materialHandleCache[assetId] = resolved;
    return resolved;
  }

  void WorldPartition::dispatchPendingLoads(uint32_t maxConcurrentLoads)
  {
    if (m_shutdown || maxConcurrentLoads == 0)
      return;

    static const uint32_t scopeId = registerScope("Streaming/Load");
    const uint32_t limit = maxConcurrentLoads;
    while (m_inFlightLoads < limit && !m_pendingLoads.empty())
    {
      const SectorCoord coord = m_pendingLoads.front();
      m_pendingLoads.pop_front();

      Sector* sector = findSector(coord);
      if (!sector || sector->state != SectorLoadState::Queued)
        continue;

      sector->state = SectorLoadState::Loading;
      const uint32_t requestId = sector->requestId;
      const WorldPartitionConfig config = m_config;
      WorldPartition* self = this;
      m_inFlightLoads++;

      jobs().DispatchAsync([self, config, coord, requestId](const JobContext&)
      {
        SectorLoadResult result{};
        result.coord = coord;
        result.requestId = requestId;
        result.ioStart = nowTicks();

        std::vector<SpawnRecord> spawns;
        if (!self->readSectorFile(coord, spawns))
          generateSectorSpawnsStatic(config, coord, spawns);

        result.ioEnd = nowTicks();
        result.spawns = std::move(spawns);
        self->m_completedLoads.push(std::move(result));
      }, scopeId);
    }
  }

  void WorldPartition::pumpCompletedLoads(World& world,
                                          const WorldPartitionBudget& budget,
                                          uint32_t maxActivationsPerFrame)
  {
    const Tick start = nowTicks();
    uint32_t completed = 0;
    double totalLoadMs = 0.0;
    float maxLoadMs = 0.0f;

    SectorLoadResult result{};
    while (m_completedLoads.pop(result))
    {
      if (m_inFlightLoads > 0)
        m_inFlightLoads--;

      Sector* sector = findSector(result.coord);
      if (!sector)
        continue;
      if (sector->requestId != result.requestId)
        continue;

      const double loadMs = ticksToSeconds(result.ioEnd - result.ioStart) * 1000.0;
      totalLoadMs += loadMs;
      if (loadMs > maxLoadMs)
        maxLoadMs = static_cast<float>(loadMs);

      sector->spawns = std::move(result.spawns);
      sector->ioStart = result.ioStart;
      sector->ioEnd = result.ioEnd;

      completed++;

      if (!isSectorDesired(sector->coord))
      {
        markUnloaded(*sector);
        continue;
      }

      markReady(*sector);
    }

    m_frameStats.completedLoads = completed;
    m_frameStats.maxLoadMs = maxLoadMs;
    m_frameStats.avgLoadMs = (completed > 0) ? static_cast<float>(totalLoadMs / completed) : 0.0f;

    uint32_t activationLimit = maxActivationsPerFrame == 0 ? 0xFFFFFFFFu : maxActivationsPerFrame;
    uint32_t activations = 0;
    uint32_t spawned = 0;

    m_scratchReady.clear();
    for (const auto& [coord, sector] : m_sectors)
    {
      if (sector.state == SectorLoadState::ReadyToActivate && isSectorDesired(coord))
        m_scratchReady.push_back(coord);
    }

    std::sort(m_scratchReady.begin(), m_scratchReady.end(),
      [&](const SectorCoord& a, const SectorCoord& b)
      {
        const int da = sectorDistanceSq(a, m_lastCameraSector);
        const int db = sectorDistanceSq(b, m_lastCameraSector);
        if (da != db) return da < db;
        return coordLess(a, b);
      });

    for (const SectorCoord& coord : m_scratchReady)
    {
      if (activations >= activationLimit)
        break;

      Sector* sector = findSector(coord);
      if (!sector || sector->state != SectorLoadState::ReadyToActivate)
        continue;

      const uint32_t sectorCost = static_cast<uint32_t>(sector->spawns.size());
      if (budget.maxEntitiesBudget > 0 && (m_activeEntityEstimate + sectorCost) > budget.maxEntitiesBudget)
      {
        m_frameStats.rejectedByEntityBudget++;
        continue;
      }

      sector->entities.clear();
      sector->entities.reserve(sector->spawns.size());

      for (const SpawnRecord& rec : sector->spawns)
      {
        Entity e = world.create();
        Transform& t = world.add<Transform>(e);
        setLocal(t, rec.position, rec.rotation, rec.scale);

        RenderMesh& rm = world.add<RenderMesh>(e);
        rm.meshId = resolveMeshHandle(rec.meshAssetId);
        rm.materialId = resolveMaterialHandle(rec.materialAssetId);

        WorldSector& ws = world.add<WorldSector>(e);
        ws.coord = coord;
        ws.active = true;

        Bounds& b = world.add<Bounds>(e);
        b.localAabb = rec.localBounds;

        Collider& col = world.add<Collider>(e);
        col.type = ColliderType::Box;
        col.halfExtents[0] = (rec.localBounds.max.x - rec.localBounds.min.x) * 0.5f;
        col.halfExtents[1] = (rec.localBounds.max.y - rec.localBounds.min.y) * 0.5f;
        col.halfExtents[2] = (rec.localBounds.max.z - rec.localBounds.min.z) * 0.5f;

        RigidBody& rb = world.add<RigidBody>(e);
        rb.type = RigidBodyType::Static;
        rb.mass = 0.0f;

        setName(world.add<Name>(e), rec.name);

        sector->entities.push_back(e);
        spawned++;
      }

      markActive(*sector, sectorCost);
      activations++;
    }

    m_frameStats.entitiesSpawned += spawned;
    m_frameStats.activations = activations;
    m_frameStats.loadedThisFrame = static_cast<uint32_t>(m_loadedThisFrame.size());
    m_frameStats.loadedSectors = m_activeSectorCount;
    m_frameStats.estimatedSectorEntities = m_activeEntityEstimate;
    m_frameStats.pumpLoadsMs = static_cast<float>(ticksToSeconds(nowTicks() - start) * 1000.0);
  }

  void WorldPartition::pumpUnloadQueue(World& world, uint32_t maxDespawnsPerFrame)
  {
    const Tick start = nowTicks();
    const uint32_t limit = maxDespawnsPerFrame == 0 ? 0xFFFFFFFFu : maxDespawnsPerFrame;
    uint32_t despawned = 0;

    while (despawned < limit && !m_pendingDespawns.empty())
    {
      const PendingDespawn pd = m_pendingDespawns.front();
      m_pendingDespawns.pop_front();

      const bool wasTraffic = world.has<TrafficAgent>(pd.entity);
      if (world.destroy(pd.entity))
      {
        despawned++;
        if (!wasTraffic && m_activeEntityEstimate > 0)
          m_activeEntityEstimate--;
      }
      else
      {
        if (!wasTraffic && m_activeEntityEstimate > 0)
          m_activeEntityEstimate--;
      }

      Sector* sector = findSector(pd.coord);
      if (sector && sector->pendingDespawns > 0)
      {
        sector->pendingDespawns--;
        if (sector->pendingDespawns == 0 && sector->state == SectorLoadState::Unloading)
        {
          markUnloaded(*sector);
        }
      }
    }

    m_frameStats.entitiesDespawned += despawned;
    m_frameStats.despawns = despawned;
    m_frameStats.unloadedThisFrame = static_cast<uint32_t>(m_unloadedThisFrame.size());
    m_frameStats.loadedSectors = m_activeSectorCount;
    m_frameStats.estimatedSectorEntities = m_activeEntityEstimate;
    m_frameStats.pumpUnloadsMs = static_cast<float>(ticksToSeconds(nowTicks() - start) * 1000.0);

    uint32_t queued = 0;
    uint32_t loading = 0;
    uint32_t ready = 0;
    uint32_t active = 0;
    uint32_t unloading = 0;
    for (const auto& [coord, sector] : m_sectors)
    {
      (void)coord;
      switch (sector.state)
      {
        case SectorLoadState::Queued: queued++; break;
        case SectorLoadState::Loading: loading++; break;
        case SectorLoadState::ReadyToActivate: ready++; break;
        case SectorLoadState::Active: active++; break;
        case SectorLoadState::Unloading: unloading++; break;
        default: break;
      }
    }

    m_frameStats.queuedSectors = queued;
    m_frameStats.loadingSectors = loading;
    m_frameStats.readySectors = ready;
    m_frameStats.activeSectors = active;
    m_frameStats.unloadingSectors = unloading;
  }

  void WorldPartition::shutdownStreaming()
  {
    m_shutdown = true;

    m_pendingLoads.clear();
    for (auto& [coord, sector] : m_sectors)
    {
      (void)coord;
      if (sector.state == SectorLoadState::Queued ||
          sector.state == SectorLoadState::Loading ||
          sector.state == SectorLoadState::ReadyToActivate)
      {
        sector.requestId++;
        sector.state = SectorLoadState::Unloaded;
        sector.spawns.clear();
        sector.entities.clear();
        sector.pendingDespawns = 0;
      }
    }

    while (m_inFlightLoads > 0)
    {
      SectorLoadResult result{};
      while (m_completedLoads.pop(result))
      {
        if (m_inFlightLoads > 0)
          m_inFlightLoads--;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    m_completedLoads.clear();
    m_pendingDespawns.clear();
  }

  Frustum frustumFromViewProj(const Mat4& viewProj)
  {
    const float* m = viewProj.m;
    const float r0x = m[0], r0y = m[4], r0z = m[8], r0w = m[12];
    const float r1x = m[1], r1y = m[5], r1z = m[9], r1w = m[13];
    const float r2x = m[2], r2y = m[6], r2z = m[10], r2w = m[14];
    const float r3x = m[3], r3y = m[7], r3z = m[11], r3w = m[15];

    auto normalizePlane = [](float a, float b, float c, float d)
    {
      Plane p{};
      const float lenSq = a * a + b * b + c * c;
      if (lenSq > 1e-8f)
      {
        const float invLen = 1.0f / std::sqrt(lenSq);
        p.n[0] = a * invLen;
        p.n[1] = b * invLen;
        p.n[2] = c * invLen;
        p.d = d * invLen;
      }
      return p;
    };

    Frustum fr{};
    fr.planes[0] = normalizePlane(r3x + r0x, r3y + r0y, r3z + r0z, r3w + r0w); // left
    fr.planes[1] = normalizePlane(r3x - r0x, r3y - r0y, r3z - r0z, r3w - r0w); // right
    fr.planes[2] = normalizePlane(r3x + r1x, r3y + r1y, r3z + r1z, r3w + r1w); // bottom
    fr.planes[3] = normalizePlane(r3x - r1x, r3y - r1y, r3z - r1z, r3w - r1w); // top
    fr.planes[4] = normalizePlane(r3x + r2x, r3y + r2y, r3z + r2z, r3w + r2w); // near
    fr.planes[5] = normalizePlane(r3x - r2x, r3y - r2y, r3z - r2z, r3w - r2w); // far
    fr.valid = true;
    return fr;
  }

  bool sphereInFrustum(const Frustum& frustum, const float center[3], float radius)
  {
    if (!frustum.valid)
      return true;

    for (const Plane& p : frustum.planes)
    {
      const float d = p.n[0] * center[0] + p.n[1] * center[1] + p.n[2] * center[2] + p.d;
      if (d < -radius)
        return false;
    }
    return true;
  }

  void computeWorldBoundsSphere(const Transform& transform, const Bounds& bounds, float outCenter[3], float& outRadius)
  {
    const Vec3 c{
      (bounds.localAabb.min.x + bounds.localAabb.max.x) * 0.5f,
      (bounds.localAabb.min.y + bounds.localAabb.max.y) * 0.5f,
      (bounds.localAabb.min.z + bounds.localAabb.max.z) * 0.5f
    };
    const Vec3 e{
      (bounds.localAabb.max.x - bounds.localAabb.min.x) * 0.5f,
      (bounds.localAabb.max.y - bounds.localAabb.min.y) * 0.5f,
      (bounds.localAabb.max.z - bounds.localAabb.min.z) * 0.5f
    };

    const Mat4& m = transform.worldMatrix;
    outCenter[0] = m.m[0] * c.x + m.m[4] * c.y + m.m[8] * c.z + m.m[12];
    outCenter[1] = m.m[1] * c.x + m.m[5] * c.y + m.m[9] * c.z + m.m[13];
    outCenter[2] = m.m[2] * c.x + m.m[6] * c.y + m.m[10] * c.z + m.m[14];

    const float sx = std::sqrt(m.m[0] * m.m[0] + m.m[1] * m.m[1] + m.m[2] * m.m[2]);
    const float sy = std::sqrt(m.m[4] * m.m[4] + m.m[5] * m.m[5] + m.m[6] * m.m[6]);
    const float sz = std::sqrt(m.m[8] * m.m[8] + m.m[9] * m.m[9] + m.m[10] * m.m[10]);
    const float maxScale = std::max(sx, std::max(sy, sz));

    const float localRadius = std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
    outRadius = localRadius * maxScale;
  }

  void WorldStreamingSystem(World& world, float dt, void* user)
  {
    (void)dt;
    WorldStreamingState* state = static_cast<WorldStreamingState*>(user);
    if (!state)
      return;

    state->stats = {};
    state->frameIndex++;

    Transform* camTransform = nullptr;
    const Entity activeCam = pickActiveCamera(world, camTransform);
    state->cameraEntity = activeCam;

    if (!camTransform)
    {
      state->partition.clearFrameDeltas();
      state->stats.loadedSectors = state->partition.loadedSectorCount();
      state->stats.estimatedSectorEntities = state->partition.loadedEntityEstimate();
      return;
    }

    const Vec3 cameraPos{ camTransform->localPos[0], camTransform->localPos[1], camTransform->localPos[2] };
    const float yaw = camTransform->localRot[1];
    const float pitch = camTransform->localRot[0];
    Vec3 cameraForward{};
    cameraForward.x = std::sin(yaw) * std::cos(pitch);
    cameraForward.y = -std::sin(pitch);
    cameraForward.z = std::cos(yaw) * std::cos(pitch);

    state->partition.setPinnedCenters(state->pinnedCenters, state->pinnedRadius);

    WorldPartitionBudget budget{};
    budget.maxActiveSectors = state->budgets.maxActiveSectors;
    budget.maxEntitiesBudget = state->budgets.maxEntitiesBudget;
    state->partition.updateActiveSet(cameraPos,
                                     cameraForward,
                                     state->budgets.loadRadiusSectors,
                                     state->budgets.unloadRadiusSectors,
                                     budget,
                                     state->budgets.useFrustumBias,
                                     state->budgets.frustumBiasWeight,
                                     !state->freezeStreaming);

    if (!state->freezeStreaming)
      state->partition.dispatchPendingLoads(state->budgets.maxConcurrentLoads);

    state->partition.pumpCompletedLoads(world, budget, state->budgets.maxActivationsPerFrame);
    state->partition.pumpUnloadQueue(world, state->budgets.maxDespawnsPerFrame);

    state->stats = state->partition.frameStats();
  }

  void CullingSystem(World& world, float dt, void* user)
  {
    (void)dt;
    CullingState* state = static_cast<CullingState*>(user);
    if (!state || !state->frame)
      return;

    state->candidates.clear();
    world.ForEach<Transform, RenderMesh>([&](Entity e, Transform&, RenderMesh&)
    {
      state->candidates.push_back(e);
    });

    const uint32_t total = static_cast<uint32_t>(state->candidates.size());
    state->stats.renderablesTotal = total;

    state->visible.clear();
    state->culled.clear();
    state->visible.reserve(total);
    state->culled.reserve(total);

    if (total == 0)
    {
      state->stats.visible = 0;
      state->stats.culled = 0;
      return;
    }

    if (state->freezeCulling)
    {
      state->visible.insert(state->visible.end(), state->candidates.begin(), state->candidates.end());
      state->stats.visible = static_cast<uint32_t>(state->visible.size());
      state->stats.culled = 0;
      return;
    }

    state->frustum = frustumFromViewProj(state->frame->viewProj);
    if (state->visibilityMask.size() < total)
      state->visibilityMask.resize(total);

    const Frustum frustum = state->frustum;
    auto handle = jobs().Dispatch(total, 128u, [&](const JobContext& ctx)
    {
      for (uint32_t i = ctx.start; i < ctx.end; ++i)
      {
        const Entity e = state->candidates[i];
        const Transform* t = world.get<Transform>(e);
        if (!t)
        {
          state->visibilityMask[i] = 0;
          continue;
        }

        if (!world.has<Bounds>(e))
        {
          state->visibilityMask[i] = 1;
          continue;
        }

        const Bounds* bounds = world.get<Bounds>(e);
        if (!bounds)
        {
          state->visibilityMask[i] = 1;
          continue;
        }

        float center[3]{};
        float radius = 0.0f;
        computeWorldBoundsSphere(*t, *bounds, center, radius);
        state->visibilityMask[i] = sphereInFrustum(frustum, center, radius) ? 1u : 0u;
      }
    });
    jobs().Wait(handle);

    for (uint32_t i = 0; i < total; ++i)
    {
      const Entity e = state->candidates[i];
      if (state->visibilityMask[i] != 0)
        state->visible.push_back(e);
      else
        state->culled.push_back(e);
    }

    state->stats.visible = static_cast<uint32_t>(state->visible.size());
    state->stats.culled = static_cast<uint32_t>(state->culled.size());
  }

  void RenderPrepStreamingSystem(World& world, float dt, void* user)
  {
    (void)dt;
    RenderPrepStreamingState* state = static_cast<RenderPrepStreamingState*>(user);
    if (!state || !state->frame)
      return;

    RenderFrameData& frame = *state->frame;
    frame.clear();

    const uint32_t maxDraws = (state->streaming ? state->streaming->budgets.maxDrawsBudget : 0u);
    uint32_t emitted = 0;
    uint32_t dropped = 0;

    if (state->assets && state->streaming)
    {
      state->assets->beginFrame(state->streaming->frameIndex);
      state->assets->setFreezeEviction(state->streaming->freezeEviction);
    }

    if (state->culling)
    {
      for (const Entity e : state->culling->visible)
      {
        Transform* t = world.get<Transform>(e);
        RenderMesh* rm = world.get<RenderMesh>(e);
        if (!t || !rm)
          continue;

        if (maxDraws > 0 && emitted >= maxDraws)
        {
          dropped++;
          continue;
        }

        pushDrawItem(frame, e, *t, *rm);
        if (state->assets)
        {
          state->assets->touchMaterial(rm->materialId);
          state->assets->touchMesh(rm->meshId);
        }
        emitted++;
      }
    }
    else
    {
      world.ForEach<Transform, RenderMesh>([&](Entity e, Transform& t, RenderMesh& rm)
      {
        if (maxDraws > 0 && emitted >= maxDraws)
        {
          dropped++;
          return;
        }
        pushDrawItem(frame, e, t, rm);
        if (state->assets)
        {
          state->assets->touchMaterial(rm.materialId);
          state->assets->touchMesh(rm.meshId);
        }
        emitted++;
      });
    }

    if (state->assets)
    {
      const uint32_t loadLimit = state->assets->residencyConfig().maxTextureLoadsPerFrame;
      state->assets->pumpTextureLoads(loadLimit);
      if (!state->assets->residencyConfig().freezeEviction)
        state->assets->evictIfNeeded();
    }

    state->stats.drawsEmitted = emitted;
    state->stats.drawsDroppedByBudget = dropped;
  }
}
