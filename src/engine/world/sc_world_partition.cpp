#include "sc_world_partition.h"

#include "sc_jobs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sc
{
  namespace
  {
    static constexpr float kPi = 3.1415926535f;
    static constexpr AABB kUnitCubeBounds{ { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };

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
    const AABB bounds = sectorBounds(sector.coord);
    const float size = m_config.sectorSizeMeters;
    const float centerX = bounds.min.x + size * 0.5f;
    const float centerZ = bounds.min.z + size * 0.5f;

    uint32_t rng = hashCoordSeed(m_config.seed, sector.coord);
    const uint32_t countRange = m_config.propsPerSectorMax - m_config.propsPerSectorMin + 1u;
    const uint32_t propCount = m_config.propsPerSectorMin + (countRange > 0 ? (mix32(rng) % countRange) : 0u);

    sector.spawns.clear();
    sector.spawns.reserve(propCount + (m_config.includeGroundPlane ? 1u : 0u));

    if (m_config.includeGroundPlane)
    {
      SpawnRecord ground{};
      std::snprintf(ground.name, Name::kMax, "Ground_%d_%d", sector.coord.x, sector.coord.z);
      ground.position[0] = centerX;
      ground.position[1] = -0.55f;
      ground.position[2] = centerZ;
      ground.scale[0] = size * 0.5f;
      ground.scale[1] = 0.10f;
      ground.scale[2] = size * 0.5f;
      ground.meshId = 1u;
      ground.materialId = 2u; // default unlit material
      ground.localBounds = kUnitCubeBounds;
      sector.spawns.push_back(ground);
    }

    const float pad = 1.0f;
    for (uint32_t i = 0; i < propCount; ++i)
    {
      SpawnRecord rec{};
      std::snprintf(rec.name, Name::kMax, "Prop_%d_%d_%u", sector.coord.x, sector.coord.z, i);

      const float x = lerp(bounds.min.x + pad, bounds.max.x - pad, rand01(rng));
      const float z = lerp(bounds.min.z + pad, bounds.max.z - pad, rand01(rng));

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
      rec.materialId = (m < 0.40f) ? 0u : ((m < 0.80f) ? 1u : 2u);
      rec.meshId = (rand01(rng) < 0.90f) ? 1u : 0u;
      rec.localBounds = kUnitCubeBounds;

      sector.spawns.push_back(rec);
    }
  }

  void WorldPartition::markLoaded(Sector& sector)
  {
    if (sector.state == SectorLoadState::Loaded)
    {
      sector.lastTouchedFrame = m_frameCounter;
      return;
    }

    if (sector.spawns.empty())
      generateSectorSpawns(sector);

    sector.state = SectorLoadState::Loaded;
    sector.lastTouchedFrame = m_frameCounter;
    m_loadedSectorCount++;
    m_loadedEntityEstimate += static_cast<uint32_t>(sector.spawns.size());
    auto unloadedIt = std::find(m_unloadedThisFrame.begin(), m_unloadedThisFrame.end(), sector.coord);
    if (unloadedIt != m_unloadedThisFrame.end())
      m_unloadedThisFrame.erase(unloadedIt);
    m_loadedThisFrame.push_back(sector.coord);
  }

  void WorldPartition::markUnloaded(Sector& sector)
  {
    if (sector.state != SectorLoadState::Loaded)
      return;

    sector.state = SectorLoadState::Unloaded;
    if (m_loadedSectorCount > 0)
      m_loadedSectorCount--;

    const uint32_t cost = static_cast<uint32_t>(sector.spawns.size());
    if (m_loadedEntityEstimate > cost)
      m_loadedEntityEstimate -= cost;
    else
      m_loadedEntityEstimate = 0;

    auto loadedIt = std::find(m_loadedThisFrame.begin(), m_loadedThisFrame.end(), sector.coord);
    if (loadedIt != m_loadedThisFrame.end())
      m_loadedThisFrame.erase(loadedIt);
    m_unloadedThisFrame.push_back(sector.coord);
  }

  Sector& WorldPartition::ensureSectorLoaded(const SectorCoord& coord)
  {
    Sector& sector = getOrCreateSector(coord);
    markLoaded(sector);
    return sector;
  }

  bool WorldPartition::unloadSector(const SectorCoord& coord)
  {
    Sector* sector = findSector(coord);
    if (!sector)
      return false;
    if (sector->state != SectorLoadState::Loaded)
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
  }

  void WorldPartition::updateActiveSet(const Vec3& cameraPos, uint32_t radiusInSectors, const WorldPartitionBudget& budget)
  {
    m_frameCounter++;
    clearFrameDeltas();

    const SectorCoord cameraSector = worldToSector(cameraPos);
    m_frameStats.cameraSector = cameraSector;
    m_frameStats.activeRadiusSectors = radiusInSectors;

    m_scratchDesired.clear();
    const int32_t r = static_cast<int32_t>(radiusInSectors);
    const uint32_t side = static_cast<uint32_t>(r * 2 + 1);
    m_scratchDesired.reserve(static_cast<size_t>(side) * static_cast<size_t>(side));

    for (int32_t dz = -r; dz <= r; ++dz)
    {
      for (int32_t dx = -r; dx <= r; ++dx)
      {
        m_scratchDesired.push_back({ cameraSector.x + dx, cameraSector.z + dz });
      }
    }

    std::sort(m_scratchDesired.begin(), m_scratchDesired.end(),
      [&](const SectorCoord& a, const SectorCoord& b)
      {
        const int da = sectorDistanceSq(a, cameraSector);
        const int db = sectorDistanceSq(b, cameraSector);
        if (da != db) return da < db;
        return coordLess(a, b);
      });

    m_frameStats.desiredSectors = static_cast<uint32_t>(m_scratchDesired.size());

    m_scratchDesiredLookup = m_scratchDesired;
    std::sort(m_scratchDesiredLookup.begin(), m_scratchDesiredLookup.end(),
      [&](const SectorCoord& a, const SectorCoord& b)
      {
        return coordLess(a, b);
      });

    auto isDesired = [&](const SectorCoord& coord)
    {
      return std::binary_search(m_scratchDesiredLookup.begin(),
                                m_scratchDesiredLookup.end(),
                                coord,
                                [&](const SectorCoord& a, const SectorCoord& b)
                                {
                                  return coordLess(a, b);
                                });
    };

    for (const SectorCoord& coord : m_scratchDesired)
    {
      Sector& sector = getOrCreateSector(coord);
      if (sector.state == SectorLoadState::Loaded)
      {
        sector.lastTouchedFrame = m_frameCounter;
        continue;
      }

      if (sector.spawns.empty())
        generateSectorSpawns(sector);
      const uint32_t sectorCost = static_cast<uint32_t>(sector.spawns.size());

      if (budget.maxActiveSectors > 0 && m_loadedSectorCount >= budget.maxActiveSectors)
      {
        m_frameStats.rejectedBySectorBudget++;
        continue;
      }

      if (budget.maxEntitiesBudget > 0 && (m_loadedEntityEstimate + sectorCost) > budget.maxEntitiesBudget)
      {
        m_frameStats.rejectedByEntityBudget++;
        continue;
      }

      markLoaded(sector);
    }

    m_scratchUnload.clear();
    m_scratchUnload.reserve(m_loadedSectorCount);
    for (const auto& [coord, sector] : m_sectors)
    {
      if (sector.state != SectorLoadState::Loaded)
        continue;
      if (isDesired(coord))
        continue;
      m_scratchUnload.push_back(coord);
    }

    std::sort(m_scratchUnload.begin(), m_scratchUnload.end(),
      [&](const SectorCoord& a, const SectorCoord& b)
      {
        const int da = sectorDistanceSq(a, cameraSector);
        const int db = sectorDistanceSq(b, cameraSector);
        if (da != db) return da > db;
        return coordLess(a, b);
      });

    for (const SectorCoord& coord : m_scratchUnload)
      unloadSector(coord);

    const bool needSectorTrim = (budget.maxActiveSectors > 0 && m_loadedSectorCount > budget.maxActiveSectors);
    const bool needEntityTrim = (budget.maxEntitiesBudget > 0 && m_loadedEntityEstimate > budget.maxEntitiesBudget);
    if (needSectorTrim || needEntityTrim)
    {
      m_scratchLoaded.clear();
      m_scratchLoaded.reserve(m_loadedSectorCount);
      for (const auto& [coord, sector] : m_sectors)
      {
        if (sector.state == SectorLoadState::Loaded)
          m_scratchLoaded.push_back(coord);
      }

      std::sort(m_scratchLoaded.begin(), m_scratchLoaded.end(),
        [&](const SectorCoord& a, const SectorCoord& b)
        {
          if (a == cameraSector) return false;
          if (b == cameraSector) return true;

          const int da = sectorDistanceSq(a, cameraSector);
          const int db = sectorDistanceSq(b, cameraSector);
          if (da != db) return da > db;

          const Sector* sa = findSector(a);
          const Sector* sb = findSector(b);
          const uint64_t ta = sa ? sa->lastTouchedFrame : 0;
          const uint64_t tb = sb ? sb->lastTouchedFrame : 0;
          if (ta != tb) return ta < tb;
          return coordLess(a, b);
        });

      for (const SectorCoord& coord : m_scratchLoaded)
      {
        const bool sectorOk = (budget.maxActiveSectors == 0 || m_loadedSectorCount <= budget.maxActiveSectors);
        const bool entityOk = (budget.maxEntitiesBudget == 0 || m_loadedEntityEstimate <= budget.maxEntitiesBudget);
        if (sectorOk && entityOk)
          break;
        if (coord == cameraSector)
          continue;
        unloadSector(coord);
      }
    }

    m_frameStats.loadedSectors = m_loadedSectorCount;
    m_frameStats.loadedThisFrame = static_cast<uint32_t>(m_loadedThisFrame.size());
    m_frameStats.unloadedThisFrame = static_cast<uint32_t>(m_unloadedThisFrame.size());
    m_frameStats.estimatedSectorEntities = m_loadedEntityEstimate;
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
    state->stats.cameraSector = state->partition.worldToSector(cameraPos);
    state->stats.activeRadiusSectors = state->budgets.activeRadiusSectors;
    const uint32_t side = state->budgets.activeRadiusSectors * 2u + 1u;
    state->stats.desiredSectors = side * side;

    if (state->freezeStreaming)
    {
      state->partition.clearFrameDeltas();
      state->stats.loadedSectors = state->partition.loadedSectorCount();
      state->stats.estimatedSectorEntities = state->partition.loadedEntityEstimate();
      return;
    }

    WorldPartitionBudget budget{};
    budget.maxActiveSectors = state->budgets.maxActiveSectors;
    budget.maxEntitiesBudget = state->budgets.maxEntitiesBudget;
    state->partition.updateActiveSet(cameraPos, state->budgets.activeRadiusSectors, budget);

    uint32_t despawned = 0;
    for (const SectorCoord& coord : state->partition.unloadedThisFrameCoords())
    {
      Sector* sector = state->partition.findSector(coord);
      if (!sector)
        continue;
      for (const Entity e : sector->entities)
      {
        if (world.destroy(e))
          despawned++;
      }
      sector->entities.clear();
    }

    uint32_t spawned = 0;
    for (const SectorCoord& coord : state->partition.loadedThisFrameCoords())
    {
      Sector* sector = state->partition.findSector(coord);
      if (!sector)
        continue;

      sector->entities.clear();
      sector->entities.reserve(sector->spawns.size());

      for (const SpawnRecord& rec : sector->spawns)
      {
        Entity e = world.create();
        Transform& t = world.add<Transform>(e);
        setLocal(t, rec.position, rec.rotation, rec.scale);

        RenderMesh& rm = world.add<RenderMesh>(e);
        rm.meshId = rec.meshId;
        rm.materialId = rec.materialId;

        WorldSector& ws = world.add<WorldSector>(e);
        ws.coord = coord;
        ws.active = true;

        Bounds& b = world.add<Bounds>(e);
        b.localAabb = rec.localBounds;

        setName(world.add<Name>(e), rec.name);

        sector->entities.push_back(e);
        spawned++;
      }
    }

    state->stats = state->partition.frameStats();
    state->stats.entitiesSpawned = spawned;
    state->stats.entitiesDespawned = despawned;
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
        emitted++;
      });
    }

    state->stats.drawsEmitted = emitted;
    state->stats.drawsDroppedByBudget = dropped;
  }
}
