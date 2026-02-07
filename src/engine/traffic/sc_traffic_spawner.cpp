#include "sc_traffic_spawner.h"

#include "sc_log.h"
#include "sc_time.h"

#include <algorithm>
#include <cmath>

namespace sc
{
  namespace
  {
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

    static Entity pickPlayerVehicle(World& world, VehicleDebugState* debug, bool& warned)
    {
      if (debug && isValidEntity(debug->activeVehicle) &&
          world.isAlive(debug->activeVehicle) &&
          world.has<PlayerVehicle>(debug->activeVehicle))
      {
        return debug->activeVehicle;
      }

      Entity active = kInvalidEntity;
      world.ForEach<PlayerVehicle>([&](Entity e, PlayerVehicle&)
      {
        if (!isValidEntity(active))
          active = e;
      });

      if (debug)
        debug->activeVehicle = active;

      if (!isValidEntity(active) && !warned)
      {
        warned = true;
        sc::log(sc::LogLevel::Warn, "PlayerVehicle not found; traffic spawner skipping player exclusion.");
      }

      return active;
    }

    static float distanceSq2d(const float a[3], const float b[3])
    {
      const float dx = a[0] - b[0];
      const float dz = a[2] - b[2];
      return dx * dx + dz * dz;
    }

    static bool laneHasGap(World& world,
                           const std::vector<Entity>& trafficEntities,
                           uint32_t laneId,
                           float laneS,
                           float minGap)
    {
      const float minGapAbs = std::fabs(minGap);
      for (const Entity e : trafficEntities)
      {
        TrafficAgent* agent = world.get<TrafficAgent>(e);
        if (!agent)
          continue;
        if (agent->laneId != laneId)
          continue;
        if (std::fabs(agent->laneS - laneS) < minGapAbs)
          return false;
      }
      return true;
    }

    static bool isOccupiedWorld(World& world, const float pos[3], float radius)
    {
      const float r2 = radius * radius;
      bool blocked = false;

      world.ForEach<TrafficAgent, Transform>([&](Entity, TrafficAgent&, Transform& tr)
      {
        if (blocked) return;
        if (distanceSq2d(tr.localPos, pos) < r2)
          blocked = true;
      });

      if (blocked)
        return true;

      world.ForEach<VehicleComponent, Transform>([&](Entity, VehicleComponent&, Transform& tr)
      {
        if (blocked) return;
        if (distanceSq2d(tr.localPos, pos) < r2)
          blocked = true;
      });

      return blocked;
    }

    static float yawFromDir(const float dir[3])
    {
      return std::atan2(dir[0], dir[2]);
    }
  }

  void TrafficSpawnerSystem(World& world, float dt, void* user)
  {
    (void)dt;
    TrafficSpawnerState* state = static_cast<TrafficSpawnerState*>(user);
    if (!state || !state->streaming || !state->lanes || !state->debug)
      return;

    TrafficDebugState& dbg = *state->debug;
    dbg.spawnsThisFrame = 0;
    dbg.spawnAttemptsThisFrame = 0;
    dbg.spawnRejectLaneGap = 0;
    dbg.spawnRejectOccupied = 0;
    dbg.spawnRejectLanePerFrame = 0;
    dbg.spawnRejectSectorLimit = 0;

    const WorldPartitionConfig& cfg = state->streaming->partition.config();
    const float sectorSize = cfg.sectorSizeMeters;
    const float areaKm2 = (sectorSize * sectorSize) * 1.0e-6f;

    float playerPos[3] = { 0.0f, 0.0f, 0.0f };
    bool hasPlayer = false;
    static bool warnedNoPlayer = false;
    {
      const Entity veh = pickPlayerVehicle(world, state->vehicleDebug, warnedNoPlayer);
      if (!isValidEntity(veh))
        return;
      if (isValidEntity(veh))
      {
        if (Transform* tr = world.get<Transform>(veh))
        {
          playerPos[0] = tr->localPos[0];
          playerPos[1] = tr->localPos[1];
          playerPos[2] = tr->localPos[2];
          hasPlayer = true;
        }
      }

      (void)hasPlayer;
    }

    const float exclusion = std::max(0.0f, dbg.playerExclusionRadius);
    const float exclusionSq = exclusion * exclusion;

    uint32_t totalTraffic = world.componentCount<TrafficAgent>();
    const uint32_t maxTotal = dbg.maxTrafficVehiclesTotal;

    const float minSpawnGapMeters = 15.0f;
    const float occupancyRadius = 6.0f;
    const uint32_t maxSpawnsPerSectorFrame = 3u;
    const uint32_t maxAttemptsPerSpawn = 10u;

    for (const auto& pair : state->streaming->partition.sectors())
    {
      const SectorCoord coord = pair.first;
      Sector* sector = state->streaming->partition.findSector(coord);
      if (!sector || sector->state != SectorLoadState::Active)
      {
        if (sector)
          state->lanes->removeSector(coord);
        continue;
      }

      state->lanes->buildProceduralForSector(coord, state->streaming->partition.sectorBounds(coord), cfg.seed);

      const std::vector<uint32_t>* lanes = state->lanes->lanesForSector(coord);
      if (!lanes || lanes->empty())
      {
        sector->trafficSpawned = true;
        continue;
      }

      const float density = std::max(0.0f, dbg.densityPerKm2);
      const uint32_t desired = static_cast<uint32_t>(std::floor(density * areaKm2 + 0.0001f));
      const uint32_t current = static_cast<uint32_t>(sector->trafficEntities.size());

      if (desired == 0)
      {
        sector->trafficSpawned = true;
        continue;
      }

      if (current >= desired)
      {
        sector->trafficSpawned = true;
        continue;
      }

      if (maxTotal > 0 && totalTraffic >= maxTotal)
        continue;

      uint32_t rng = hashCoordSeed(cfg.seed, coord);
      uint32_t spawned = 0;
      const uint32_t toSpawn = desired - current;
      uint32_t spawnsThisSector = 0;
      std::vector<uint32_t> lanesSpawnedThisFrame;
      lanesSpawnedThisFrame.reserve(lanes->size());

      sector->trafficEntities.reserve(sector->trafficEntities.size() + toSpawn);

      for (uint32_t i = 0; i < toSpawn; ++i)
      {
        if (maxTotal > 0 && totalTraffic >= maxTotal)
          break;
        if (spawnsThisSector >= maxSpawnsPerSectorFrame)
        {
          dbg.spawnRejectSectorLimit++;
          break;
        }

        bool placed = false;
        uint32_t laneId = kInvalidLaneId;
        float laneS = 0.0f;
        float dir[3] = { 0.0f, 0.0f, 1.0f };
        float pos[3] = { 0.0f, 0.35f, 0.0f };

        for (uint32_t attempt = 0; attempt < maxAttemptsPerSpawn; ++attempt)
        {
          dbg.spawnAttemptsThisFrame++;
          const uint32_t laneIndex = static_cast<uint32_t>(rand01(rng) * lanes->size()) % static_cast<uint32_t>(lanes->size());
          laneId = (*lanes)[laneIndex];
          const LaneSegment* seg = state->lanes->getLane(laneId);
          if (!seg || !seg->active || seg->length <= 1e-4f)
            continue;

          const LaneNode* start = state->lanes->getNode(seg->startNode);
          if (!start)
            continue;

          bool laneSpawned = false;
          for (const uint32_t spawnedLane : lanesSpawnedThisFrame)
          {
            if (spawnedLane == laneId)
            {
              laneSpawned = true;
              break;
            }
          }
          if (laneSpawned)
          {
            dbg.spawnRejectLanePerFrame++;
            continue;
          }

          laneS = rand01(rng) * seg->length;
          dir[0] = seg->dir[0];
          dir[1] = seg->dir[1];
          dir[2] = seg->dir[2];

          pos[0] = start->pos[0] + dir[0] * laneS;
          pos[1] = 0.35f;
          pos[2] = start->pos[2] + dir[2] * laneS;

          if (!laneHasGap(world, sector->trafficEntities, laneId, laneS, minSpawnGapMeters))
          {
            dbg.spawnRejectLaneGap++;
            continue;
          }

          if (isOccupiedWorld(world, pos, occupancyRadius))
          {
            dbg.spawnRejectOccupied++;
            continue;
          }

          if (!hasPlayer || distanceSq2d(pos, playerPos) > exclusionSq)
          {
            placed = true;
            break;
          }
        }

        if (!placed || laneId == kInvalidLaneId)
          continue;

        Entity e = world.create();
        Transform& tr = world.add<Transform>(e);
        float rot[3] = { 0.0f, yawFromDir(dir), 0.0f };
        setLocal(tr, pos, rot, state->vehicleScale);

        RenderMesh& rm = world.add<RenderMesh>(e);
        rm.meshId = state->meshId;
        rm.materialId = state->materialId;

        Bounds& b = world.add<Bounds>(e);
        b.localAabb = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };

        WorldSector& ws = world.add<WorldSector>(e);
        ws.coord = coord;
        ws.active = true;

        TrafficAgent& agent = world.add<TrafficAgent>(e);
        agent.laneId = laneId;
        agent.laneS = laneS;
        agent.lookAheadDist = dbg.lookAheadDist;
        agent.targetSpeed = 0.0f;

        TrafficVehicle& tv = world.add<TrafficVehicle>(e);
        tv.mode = TrafficSimMode::OnRails;

        TrafficSensors& sensors = world.add<TrafficSensors>(e);
        sensors.frontRayLength = dbg.frontRayLength;
        sensors.safeDistance = dbg.safeDistance;
        sensors.sideRayLength = 6.0f;

        setName(world.add<Name>(e), "Traffic");

        sector->trafficEntities.push_back(e);
        lanesSpawnedThisFrame.push_back(laneId);
        spawned++;
        spawnsThisSector++;
        totalTraffic++;
      }

      dbg.spawnsThisFrame += spawned;
      sector->trafficSpawned = (current + spawned) >= desired;
    }

    const Tick now = nowTicks();
    const double elapsed = ticksToSeconds(now - state->lastLogTicks);
    if (elapsed > 1.0)
    {
      if (dbg.spawnsThisFrame > 0)
        sc::log(sc::LogLevel::Info, "Traffic spawns this frame: %u", dbg.spawnsThisFrame);
      state->lastLogTicks = now;
    }
  }
}
