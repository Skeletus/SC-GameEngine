#include "sc_traffic_lod.h"

#include "sc_physics.h"
#include "sc_log.h"
#include <algorithm>
#include <cmath>

namespace sc
{
  namespace
  {
    static float distanceSq2d(const float a[3], const float b[3])
    {
      const float dx = a[0] - b[0];
      const float dz = a[2] - b[2];
      return dx * dx + dz * dz;
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
        sc::log(sc::LogLevel::Warn, "PlayerVehicle not found; skipping traffic LOD update.");
      }

      return active;
    }

    static void ensureRenderMesh(World& world, Entity e, uint32_t meshId, uint32_t materialId)
    {
      RenderMesh* rm = world.get<RenderMesh>(e);
      if (!rm)
      {
        RenderMesh& added = world.add<RenderMesh>(e);
        added.meshId = meshId;
        added.materialId = materialId;
        return;
      }
      rm->meshId = meshId;
      rm->materialId = materialId;
    }

    static void ensureBounds(World& world, Entity e)
    {
      Bounds* b = world.get<Bounds>(e);
      if (!b)
      {
        Bounds& added = world.add<Bounds>(e);
        added.localAabb = { { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f } };
      }
    }

    static void applyTrafficTuning(VehicleComponent& vc)
    {
      vc.mass = 1200.0f;
      vc.engineForce = 9000.0f;
      vc.maxSpeed = 45.0f;
      vc.brakeForce = 12000.0f;
      vc.handbrakeForce = 8000.0f;
      vc.maxSteerAngle = 0.55f;
      vc.steerResponse = 6.0f;
      vc.suspensionRestLength = 0.35f;
      vc.suspensionStiffness = 20.0f;
      vc.dampingCompression = 2.3f;
      vc.dampingRelaxation = 4.4f;
      vc.wheelRadius = 0.35f;
      vc.wheelWidth = 0.25f;
      vc.centerOfMassOffset[0] = 0.0f;
      vc.centerOfMassOffset[1] = 0.0f;
      vc.centerOfMassOffset[2] = 0.0f;
    }

    static void ensureDynamicBody(World& world, Entity e)
    {
      RigidBody* rb = world.get<RigidBody>(e);
      if (!rb)
      {
        RigidBody& added = world.add<RigidBody>(e);
        added.type = RigidBodyType::Dynamic;
        added.mass = 1200.0f;
        added.friction = 0.9f;
        added.angularDamping = 0.2f;
      }
      else
      {
        rb->type = RigidBodyType::Dynamic;
        rb->mass = 1200.0f;
        rb->friction = 0.9f;
        rb->angularDamping = 0.2f;
      }

      Collider* col = world.get<Collider>(e);
      if (!col)
      {
        Collider& added = world.add<Collider>(e);
        added.type = ColliderType::Box;
        added.halfExtents[0] = 0.5f;
        added.halfExtents[1] = 0.5f;
        added.halfExtents[2] = 0.5f;
        added.layer = 1u;
        added.mask = 0xFFFFFFFFu;
      }
      else
      {
        col->type = ColliderType::Box;
        col->halfExtents[0] = 0.5f;
        col->halfExtents[1] = 0.5f;
        col->halfExtents[2] = 0.5f;
        col->layer = 1u;
        col->mask = 0xFFFFFFFFu;
      }
    }

    static void ensureKinematicBody(World& world, Entity e)
    {
      RigidBody* rb = world.get<RigidBody>(e);
      if (!rb)
      {
        RigidBody& added = world.add<RigidBody>(e);
        added.type = RigidBodyType::Kinematic;
        added.mass = 1200.0f;
        added.friction = 0.9f;
        added.angularDamping = 0.2f;
      }
      else
      {
        rb->type = RigidBodyType::Kinematic;
        rb->mass = 1200.0f;
        rb->friction = 0.9f;
        rb->angularDamping = 0.2f;
      }

      Collider* col = world.get<Collider>(e);
      if (!col)
      {
        Collider& added = world.add<Collider>(e);
        added.type = ColliderType::Box;
        added.halfExtents[0] = 0.5f;
        added.halfExtents[1] = 0.5f;
        added.halfExtents[2] = 0.5f;
        added.layer = 1u;
        added.mask = 0u;
      }
      else
      {
        col->type = ColliderType::Box;
        col->halfExtents[0] = 0.5f;
        col->halfExtents[1] = 0.5f;
        col->halfExtents[2] = 0.5f;
        col->layer = 1u;
        col->mask = 0u;
      }
    }

    static void removeFromSectorList(WorldStreamingState* streaming, Entity e, const SectorCoord& coord)
    {
      if (!streaming)
        return;
      Sector* sector = streaming->partition.findSector(coord);
      if (!sector)
        return;

      auto& list = sector->trafficEntities;
      for (size_t i = 0; i < list.size(); ++i)
      {
        if (list[i] == e)
        {
          list[i] = list.back();
          list.pop_back();
          break;
        }
      }
    }

    static void applyMode(World& world,
                          Entity e,
                          TrafficVehicle& tv,
                          TrafficSimMode mode,
                          uint32_t meshId,
                          uint32_t materialId)
    {
      ensureRenderMesh(world, e, meshId, materialId);
      ensureBounds(world, e);

      if (mode == TrafficSimMode::Physics)
      {
        if (world.has<VehicleRuntime>(e))
          world.remove<VehicleRuntime>(e);
        if (world.has<PhysicsBodyHandle>(e))
          world.remove<PhysicsBodyHandle>(e);

        VehicleComponent* vc = world.get<VehicleComponent>(e);
        if (!vc)
          vc = &world.add<VehicleComponent>(e);
        applyTrafficTuning(*vc);

        if (!world.has<VehicleInput>(e))
          world.add<VehicleInput>(e);

        ensureDynamicBody(world, e);
        tv.mode = TrafficSimMode::Physics;
        tv.renderOffsetMode = 2;
        return;
      }

      if (mode == TrafficSimMode::Kinematic)
      {
        if (world.has<VehicleComponent>(e))
          world.remove<VehicleComponent>(e);
        if (world.has<VehicleInput>(e))
          world.remove<VehicleInput>(e);
        if (world.has<VehicleRuntime>(e))
          world.remove<VehicleRuntime>(e);

        ensureKinematicBody(world, e);
        tv.mode = TrafficSimMode::Kinematic;
        tv.renderOffsetMode = 0;
        return;
      }

      // On rails
      if (world.has<VehicleComponent>(e))
        world.remove<VehicleComponent>(e);
      if (world.has<VehicleInput>(e))
        world.remove<VehicleInput>(e);
      if (world.has<VehicleRuntime>(e))
        world.remove<VehicleRuntime>(e);
      if (world.has<RigidBody>(e))
        world.remove<RigidBody>(e);
      if (world.has<Collider>(e))
        world.remove<Collider>(e);
      if (world.has<PhysicsBodyHandle>(e))
        world.remove<PhysicsBodyHandle>(e);

      tv.mode = TrafficSimMode::OnRails;
      tv.vehicle = {};
      tv.body = {};
      tv.renderOffsetMode = 0;
    }
  }

  void TrafficLODSystem(World& world, float dt, void* user)
  {
    (void)dt;
    TrafficLODState* state = static_cast<TrafficLODState*>(user);
    if (!state || !state->streaming || !state->lanes || !state->debug)
      return;

    TrafficDebugState& dbg = *state->debug;

    if (dbg.tierAExit < dbg.tierAEnter + 1.0f)
      dbg.tierAExit = dbg.tierAEnter + 1.0f;
    if (dbg.tierBEnter < dbg.tierAExit + 1.0f)
      dbg.tierBEnter = dbg.tierAExit + 1.0f;
    if (dbg.tierBExit < dbg.tierBEnter + 1.0f)
      dbg.tierBExit = dbg.tierBEnter + 1.0f;

    float playerPos[3] = { 0.0f, 0.0f, 0.0f };
    {
      static bool warnedNoPlayer = false;
      const Entity veh = pickPlayerVehicle(world, state->vehicleDebug, warnedNoPlayer);
      if (!isValidEntity(veh))
        return;

      if (Transform* tr = world.get<Transform>(veh))
      {
        playerPos[0] = tr->localPos[0];
        playerPos[1] = tr->localPos[1];
        playerPos[2] = tr->localPos[2];
      }
    }

    struct Entry
    {
      Entity e{};
      float dist = 0.0f;
      TrafficVehicle* tv = nullptr;
      TrafficAgent* agent = nullptr;
      Transform* tr = nullptr;
    };

    std::vector<Entry> entries;
    entries.reserve(world.componentCount<TrafficVehicle>());

    world.ForEach<TrafficVehicle, TrafficAgent, Transform>([&](Entity e, TrafficVehicle& tv, TrafficAgent& agent, Transform& tr)
    {
      const float d = std::sqrt(distanceSq2d(tr.localPos, playerPos));
      entries.push_back({ e, d, &tv, &agent, &tr });
    });

    if (entries.empty())
    {
      dbg.totalVehicles = 0;
      dbg.tierPhysics = 0;
      dbg.tierKinematic = 0;
      dbg.tierOnRails = 0;
      dbg.despawnsThisFrame = (state->lastTotalVehicles > 0) ? state->lastTotalVehicles : 0;
      state->lastTotalVehicles = 0;
      dbg.nearestLaneId = kInvalidLaneId;
      return;
    }

    std::vector<TrafficSimMode> desired(entries.size(), TrafficSimMode::OnRails);

    for (size_t i = 0; i < entries.size(); ++i)
    {
      const float dist = entries[i].dist;
      const TrafficSimMode current = entries[i].tv->mode;

      if (current == TrafficSimMode::Physics)
      {
        if (dist > dbg.tierAExit)
          desired[i] = (dist < dbg.tierBEnter) ? TrafficSimMode::Kinematic : TrafficSimMode::OnRails;
        else
          desired[i] = TrafficSimMode::Physics;
      }
      else if (current == TrafficSimMode::Kinematic)
      {
        if (dist < dbg.tierAEnter)
          desired[i] = TrafficSimMode::Physics;
        else if (dist > dbg.tierBExit)
          desired[i] = TrafficSimMode::OnRails;
        else
          desired[i] = TrafficSimMode::Kinematic;
      }
      else
      {
        if (dist < dbg.tierAEnter)
          desired[i] = TrafficSimMode::Physics;
        else if (dist < dbg.tierBEnter)
          desired[i] = TrafficSimMode::Kinematic;
        else
          desired[i] = TrafficSimMode::OnRails;
      }
    }

    auto byDistDesc = [&](size_t a, size_t b)
    {
      return entries[a].dist > entries[b].dist;
    };

    uint32_t physicsCount = 0;
    uint32_t kinematicCount = 0;
    uint32_t onRailsCount = 0;
    for (TrafficSimMode m : desired)
    {
      if (m == TrafficSimMode::Physics) physicsCount++;
      else if (m == TrafficSimMode::Kinematic) kinematicCount++;
      else onRailsCount++;
    }

    const uint32_t maxPhysics = dbg.maxTrafficVehiclesPhysics;
    const uint32_t maxKinematic = dbg.maxTrafficVehiclesKinematic;

    if (maxPhysics > 0 && physicsCount > maxPhysics)
    {
      std::vector<size_t> phys;
      phys.reserve(physicsCount);
      for (size_t i = 0; i < desired.size(); ++i)
      {
        if (desired[i] == TrafficSimMode::Physics)
          phys.push_back(i);
      }
      std::sort(phys.begin(), phys.end(), byDistDesc);

      for (size_t i = maxPhysics; i < phys.size(); ++i)
      {
        const size_t idx = phys[i];
        if (maxKinematic == 0 || kinematicCount < maxKinematic)
        {
          desired[idx] = TrafficSimMode::Kinematic;
          kinematicCount++;
        }
        else
        {
          desired[idx] = TrafficSimMode::OnRails;
          onRailsCount++;
        }
        physicsCount--;
      }
    }

    if (maxKinematic > 0 && kinematicCount > maxKinematic)
    {
      std::vector<size_t> kin;
      kin.reserve(kinematicCount);
      for (size_t i = 0; i < desired.size(); ++i)
      {
        if (desired[i] == TrafficSimMode::Kinematic)
          kin.push_back(i);
      }
      std::sort(kin.begin(), kin.end(), byDistDesc);
      for (size_t i = maxKinematic; i < kin.size(); ++i)
      {
        desired[kin[i]] = TrafficSimMode::OnRails;
        kinematicCount--;
        onRailsCount++;
      }
    }

    std::vector<uint8_t> despawnFlags(entries.size(), 0);
    const uint32_t maxTotal = dbg.maxTrafficVehiclesTotal;
    if (maxTotal > 0 && entries.size() > maxTotal)
    {
      uint32_t toRemove = static_cast<uint32_t>(entries.size()) - maxTotal;

      auto collectByMode = [&](TrafficSimMode mode, std::vector<size_t>& out)
      {
        out.clear();
        for (size_t i = 0; i < desired.size(); ++i)
        {
          if (desired[i] == mode)
            out.push_back(i);
        }
        std::sort(out.begin(), out.end(), byDistDesc);
      };

      std::vector<size_t> bucket;
      collectByMode(TrafficSimMode::OnRails, bucket);
      for (size_t idx : bucket)
      {
        if (toRemove == 0) break;
        despawnFlags[idx] = 1;
        toRemove--;
      }

      if (toRemove > 0)
      {
        collectByMode(TrafficSimMode::Kinematic, bucket);
        for (size_t idx : bucket)
        {
          if (toRemove == 0) break;
          despawnFlags[idx] = 1;
          toRemove--;
        }
      }

      if (toRemove > 0)
      {
        collectByMode(TrafficSimMode::Physics, bucket);
        for (size_t idx : bucket)
        {
          if (toRemove == 0) break;
          despawnFlags[idx] = 1;
          toRemove--;
        }
      }
    }

    uint32_t activeTotal = 0;
    uint32_t countPhys = 0;
    uint32_t countKin = 0;
    uint32_t countRail = 0;

    for (size_t i = 0; i < entries.size(); ++i)
    {
      Entry& entry = entries[i];
      if (despawnFlags[i])
      {
        SectorCoord coord{};
        if (WorldSector* ws = world.get<WorldSector>(entry.e))
          coord = ws->coord;
        else
        {
          const Vec3 pos{ entry.tr->localPos[0], entry.tr->localPos[1], entry.tr->localPos[2] };
          coord = state->streaming->partition.worldToSector(pos);
        }

        removeFromSectorList(state->streaming, entry.e, coord);
        world.destroy(entry.e);
        continue;
      }

      activeTotal++;

      if (entry.tv->mode != desired[i])
        applyMode(world, entry.e, *entry.tv, desired[i], state->meshId, state->materialId);

      if (desired[i] == TrafficSimMode::Physics) countPhys++;
      else if (desired[i] == TrafficSimMode::Kinematic) countKin++;
      else countRail++;
    }

    dbg.totalVehicles = activeTotal;
    dbg.tierPhysics = countPhys;
    dbg.tierKinematic = countKin;
    dbg.tierOnRails = countRail;

    if (state->lastTotalVehicles + dbg.spawnsThisFrame > activeTotal)
      dbg.despawnsThisFrame = state->lastTotalVehicles + dbg.spawnsThisFrame - activeTotal;
    else
      dbg.despawnsThisFrame = 0;
    state->lastTotalVehicles = activeTotal;

    if (state->lanes)
    {
      LaneQuery q = state->lanes->queryNearestLane(playerPos);
      dbg.nearestLaneId = q.laneId;
    }
  }

  void TrafficPinSystem(World& world, float dt, void* user)
  {
    (void)dt;
    TrafficPinState* state = static_cast<TrafficPinState*>(user);
    if (!state || !state->streaming || !state->debug)
      return;

    const uint32_t pinRadius = state->debug->trafficPinRadius;
    if (pinRadius > state->streaming->pinnedRadius)
      state->streaming->pinnedRadius = pinRadius;

    world.ForEach<TrafficVehicle, Transform>([&](Entity, TrafficVehicle& tv, Transform& tr)
    {
      if (tv.mode != TrafficSimMode::Physics)
        return;

      const Vec3 pos{ tr.localPos[0], tr.localPos[1], tr.localPos[2] };
      const SectorCoord coord = state->streaming->partition.worldToSector(pos);

      bool exists = false;
      for (const SectorCoord& c : state->streaming->pinnedCenters)
      {
        if (c == coord)
        {
          exists = true;
          break;
        }
      }
      if (!exists)
        state->streaming->pinnedCenters.push_back(coord);
    });
  }
}
