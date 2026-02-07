#include "sc_traffic_ai.h"

#include "sc_debug_draw.h"
#include "sc_log.h"
#include "sc_time.h"
#include "sc_vehicle.h"
#include "sc_physics.h"

#include <algorithm>
#include <cmath>

namespace sc
{
  namespace
  {
    static float clamp(float v, float lo, float hi)
    {
      return std::max(lo, std::min(v, hi));
    }

    static float clamp01(float v)
    {
      return clamp(v, 0.0f, 1.0f);
    }

    static float length3(const float v[3])
    {
      return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    }

    static float distanceSq2d(const float a[3], const float b[3])
    {
      const float dx = a[0] - b[0];
      const float dz = a[2] - b[2];
      return dx * dx + dz * dz;
    }

    static void normalize3(float v[3])
    {
      const float len = length3(v);
      if (len > 1e-6f)
      {
        const float inv = 1.0f / len;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
      }
    }

    static void rotateVecEuler(const float rot[3], const float v[3], float out[3])
    {
      const Mat4 r = mat4_rotation_xyz(rot[0], rot[1], rot[2]);
      out[0] = r.m[0] * v[0] + r.m[4] * v[1] + r.m[8] * v[2];
      out[1] = r.m[1] * v[0] + r.m[5] * v[1] + r.m[9] * v[2];
      out[2] = r.m[2] * v[0] + r.m[6] * v[1] + r.m[10] * v[2];
    }

    static float smoothExp(float current, float target, float response, float dt)
    {
      const float t = 1.0f - std::exp(-response * dt);
      return current + (target - current) * t;
    }

    static float wrapAngle(float a)
    {
      const float twoPi = 6.283185307f;
      while (a > 3.14159265f) a -= twoPi;
      while (a < -3.14159265f) a += twoPi;
      return a;
    }

    static float yawFromDir(const float dir[3])
    {
      return std::atan2(dir[0], dir[2]);
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
        sc::log(sc::LogLevel::Warn, "PlayerVehicle not found; traffic AI skipped.");
      }

      return active;
    }

    static void updateSectorForEntity(World& world,
                                      WorldStreamingState* streaming,
                                      Entity e,
                                      const SectorCoord& newCoord)
    {
      if (!streaming)
        return;

      WorldSector* ws = world.get<WorldSector>(e);
      const SectorCoord oldCoord = ws ? ws->coord : newCoord;
      if (ws && oldCoord == newCoord)
        return;

      Sector* newSector = streaming->partition.findSector(newCoord);
      if (!newSector || newSector->state != SectorLoadState::Active)
        return;

      if (ws)
      {
        Sector* oldSector = streaming->partition.findSector(oldCoord);
        if (oldSector)
        {
          auto& list = oldSector->trafficEntities;
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
      }
      else
      {
        ws = &world.add<WorldSector>(e);
      }

      ws->coord = newCoord;
      ws->active = true;

      auto& newList = newSector->trafficEntities;
      bool exists = false;
      for (const Entity existing : newList)
      {
        if (existing == e)
        {
          exists = true;
          break;
        }
      }
      if (!exists)
        newList.push_back(e);
    }
  }

  void TrafficAISystem(World& world, float dt, void* user)
  {
    TrafficAIState* state = static_cast<TrafficAIState*>(user);
    if (!state || !state->lanes)
      return;

    TrafficLaneGraph& lanes = *state->lanes;
    TrafficDebugState* dbg = state->debug;

    float playerPos[3] = { 0.0f, 0.0f, 0.0f };
    {
      static bool warnedNoPlayer = false;
      const Entity player = pickPlayerVehicle(world, nullptr, warnedNoPlayer);
      if (!isValidEntity(player))
        return;
      Transform* playerTr = world.get<Transform>(player);
      if (!playerTr)
        return;
      playerPos[0] = playerTr->localPos[0];
      playerPos[1] = playerTr->localPos[1];
      playerPos[2] = playerTr->localPos[2];
    }

    if (dbg)
    {
      dbg->nearestTrafficEntity = kInvalidEntity;
      dbg->nearestTrafficDistance = 0.0f;
      dbg->nearestTrafficSpeed = 0.0f;
      dbg->nearestTrafficTargetSpeed = 0.0f;
      dbg->nearestTrafficLaneId = kInvalidLaneId;
      dbg->nearestTrafficLaneS = 0.0f;
      dbg->nearestTrafficInput = {};
      dbg->nearestTrafficBodyActive = false;
      dbg->nearestTrafficBodyInWorld = false;
      dbg->nearestTrafficBodyMass = 0.0f;
      dbg->nearestTrafficBodyLinVel[0] = 0.0f;
      dbg->nearestTrafficBodyLinVel[1] = 0.0f;
      dbg->nearestTrafficBodyLinVel[2] = 0.0f;
      dbg->nearestTrafficVehicleInWorld = false;
      dbg->nearestTrafficVehicleWheelCount = 0u;
      dbg->nearestTrafficVehicleSpeedKmh = 0.0f;
      dbg->nearestTrafficSensorHitDistance = 0.0f;
      dbg->nearestTrafficSensorHitType = TrafficHitType::None;
      dbg->stuckTrafficEntity = kInvalidEntity;
      dbg->stuckCount = 0u;
      dbg->nearestTrafficDesyncEntity = kInvalidEntity;
      dbg->nearestTrafficEcsPos[0] = 0.0f;
      dbg->nearestTrafficEcsPos[1] = 0.0f;
      dbg->nearestTrafficEcsPos[2] = 0.0f;
      dbg->nearestTrafficPhysPos[0] = 0.0f;
      dbg->nearestTrafficPhysPos[1] = 0.0f;
      dbg->nearestTrafficPhysPos[2] = 0.0f;
      dbg->nearestTrafficDesync = 0.0f;
      dbg->nearestTrafficDesyncTimer = 0.0f;
      dbg->nearestTrafficDesyncLogged = 0;
    }

    float nearestDistSq = 1.0e30f;

    world.ForEach<TrafficAgent, TrafficVehicle, Transform>([&](Entity e, TrafficAgent& agent, TrafficVehicle& tv, Transform& tr)
    {
      if (state->streaming)
      {
        if (WorldSector* ws = world.get<WorldSector>(e))
        {
          Sector* sector = state->streaming->partition.findSector(ws->coord);
          if (!sector || sector->state != SectorLoadState::Active)
            return;
        }
      }

      if (dbg)
        agent.lookAheadDist = dbg->lookAheadDist;

      if (TrafficSensors* sensors = world.get<TrafficSensors>(e))
      {
        if (dbg)
        {
          sensors->frontRayLength = dbg->frontRayLength;
          sensors->safeDistance = dbg->safeDistance;
        }
      }

      if (tv.mode == TrafficSimMode::Physics)
      {
        if (VehicleRuntime* rt = world.get<VehicleRuntime>(e))
          tv.vehicle = rt->handle;
        if (PhysicsBodyHandle* hb = world.get<PhysicsBodyHandle>(e))
          tv.body = *hb;
      }
      else if (tv.mode == TrafficSimMode::Kinematic)
      {
        if (PhysicsBodyHandle* hb = world.get<PhysicsBodyHandle>(e))
          tv.body = *hb;
        tv.vehicle = {};
      }
      else
      {
        tv.vehicle = {};
        tv.body = {};
      }

      if (agent.laneId == kInvalidLaneId)
      {
        LaneQuery q = lanes.queryNearestLane(tr.localPos);
        if (q.laneId != kInvalidLaneId)
        {
          agent.laneId = q.laneId;
          agent.laneS = q.s;
        }
      }

      const LaneSegment* seg = lanes.getLane(agent.laneId);
      if (!seg || !seg->active)
        return;

      float target[3]{};
      if (!lanes.getLookAheadPoint(agent.laneId, agent.laneS, agent.lookAheadDist, target))
        return;

      float toTarget[3] = { target[0] - tr.localPos[0], 0.0f, target[2] - tr.localPos[2] };
      if (length3(toTarget) < 1e-4f)
        return;
      normalize3(toTarget);

      const float desiredYaw = yawFromDir(toTarget);
      const float currentYaw = tr.localRot[1];
      const float deltaYaw = wrapAngle(desiredYaw - currentYaw);

      float maxSteer = 0.55f;
      if (VehicleComponent* vc = world.get<VehicleComponent>(e))
        maxSteer = std::max(0.1f, vc->maxSteerAngle);
      const float steer = clamp(deltaYaw / maxSteer, -1.0f, 1.0f);

      float desiredSpeed = lanes.laneSpeedLimit(agent.laneId);
      if (dbg)
        desiredSpeed *= dbg->speedMultiplier;
      desiredSpeed = std::max(0.0f, desiredSpeed);

      float obstacleBrake = 0.0f;
      if (state->physics)
      {
        float forward[3] = { std::sin(currentYaw), 0.0f, std::cos(currentYaw) };
        normalize3(forward);

        TrafficSensors* sensors = world.get<TrafficSensors>(e);
        const float rayLen = sensors ? sensors->frontRayLength : 20.0f;
        const float safe = sensors ? sensors->safeDistance : 10.0f;

        float origin[3] = {
          tr.localPos[0] + forward[0] * 1.7f,
          tr.localPos[1] + 0.6f,
          tr.localPos[2] + forward[2] * 1.7f
        };

        TrafficHitType hitType = TrafficHitType::None;
        float hitDistance = rayLen;
        RaycastHit hit = state->physics->raycast(origin, forward, rayLen, 1u);
        if (hit.hit)
        {
          hitDistance = hit.distance;
          if (hit.entity == e)
          {
            hitType = TrafficHitType::Self;
          }
          else if (world.has<VehicleComponent>(hit.entity) || world.has<VehicleRuntime>(hit.entity))
          {
            hitType = TrafficHitType::Vehicle;
          }
          else
          {
            hitType = TrafficHitType::World;
          }

          if (hitType != TrafficHitType::Self && safe > 1e-3f && hit.distance < safe)
          {
            obstacleBrake = clamp01((safe - hit.distance) / safe);
          }
        }

        if (sensors)
        {
          sensors->lastHitDistance = hitDistance;
          sensors->lastHitType = hitType;
        }
      }

      float speedMsForDebug = 0.0f;

      if (tv.mode == TrafficSimMode::Physics)
      {
        VehicleInput* input = world.get<VehicleInput>(e);
        if (!input)
          input = &world.add<VehicleInput>(e);

        float speed = agent.targetSpeed;
        if (VehicleRuntime* rt = world.get<VehicleRuntime>(e))
          speed = rt->speedMs;
        const float speedMs = speed;
        speedMsForDebug = speedMs;

        float throttle = 0.0f;
        float brake = 0.0f;
        const float speedError = desiredSpeed - speed;
        const float denom = std::max(1.0f, desiredSpeed);
        if (speedError > 0.5f)
          throttle = clamp01(speedError / denom);
        else if (speedError < -0.5f)
          brake = clamp01(-speedError / denom);

        brake = std::max(brake, obstacleBrake);
        throttle *= (1.0f - obstacleBrake);

        input->steer = steer;
        input->throttle = throttle;
        input->brake = brake;
        input->handbrake = 0.0f;

        if (state->physics && tv.body.valid() && (input->throttle > 0.05f || desiredSpeed > 2.0f))
        {
          if (!state->physics->isBodyActive(tv.body))
            state->physics->activateBody(tv.body);
        }

        if (desiredSpeed > 2.0f && speedMs < 0.2f)
        {
          agent.stuckTimer += dt;
        }
        else
        {
          agent.stuckTimer = 0.0f;
          agent.stuckLogged = 0;
        }

        if (agent.stuckTimer > 1.0f && agent.stuckLogged == 0)
        {
          static Tick lastLog = 0;
          const Tick now = nowTicks();
          const double since = ticksToSeconds(now - lastLog);
          if (since > 1.0)
          {
            lastLog = now;
            sc::log(sc::LogLevel::Warn,
                    "Traffic stuck: e=%u tier=A speed=%.2f target=%.2f lane=%u s=%.2f",
                    e.index(), speedMs, desiredSpeed, agent.laneId, agent.laneS);
          }
          agent.stuckLogged = 1;
          if (dbg)
          {
            dbg->stuckTrafficEntity = e;
            dbg->stuckTrafficTier = tv.mode;
            dbg->stuckTrafficSpeed = speedMs;
            dbg->stuckTrafficTargetSpeed = desiredSpeed;
            dbg->stuckTrafficLaneId = agent.laneId;
            dbg->stuckTrafficLaneS = agent.laneS;
            dbg->stuckTrafficBodyActive = state->physics ? state->physics->isBodyActive(tv.body) : false;
            if (TrafficSensors* sensors = world.get<TrafficSensors>(e))
            {
              dbg->stuckTrafficSensorHitDistance = sensors->lastHitDistance;
              dbg->stuckTrafficSensorHitType = sensors->lastHitType;
            }
          }
        }

        LaneQuery q = lanes.queryNearestLane(tr.localPos);
        if (q.laneId != kInvalidLaneId)
        {
          agent.laneId = q.laneId;
          agent.laneS = q.s;
        }

      }
      else
      {
        const float desired = desiredSpeed * (1.0f - obstacleBrake);
        agent.targetSpeed = smoothExp(agent.targetSpeed, desired, 2.5f, dt);
        speedMsForDebug = agent.targetSpeed;
        const float travel = agent.targetSpeed * dt;

        uint32_t laneId = agent.laneId;
        float laneS = agent.laneS;
        float pos[3]{};
        float dir[3]{};
        if (lanes.advanceAlongLane(laneId, laneS, travel, pos, dir))
        {
          pos[1] = tr.localPos[1];
          agent.laneId = laneId;
          agent.laneS = laneS;

          tr.localPos[0] = pos[0];
          tr.localPos[1] = pos[1];
          tr.localPos[2] = pos[2];
          tr.localRot[0] = 0.0f;
          tr.localRot[1] = yawFromDir(dir);
          tr.localRot[2] = 0.0f;
          tr.dirty = true;

        }
      }

      if (dbg)
      {
        const float distSq = distanceSq2d(tr.localPos, playerPos);
        if (distSq < nearestDistSq)
        {
          nearestDistSq = distSq;
          dbg->nearestTrafficEntity = e;
          dbg->nearestTrafficTier = tv.mode;
          dbg->nearestTrafficDistance = std::sqrt(distSq);
          dbg->nearestTrafficSpeed = speedMsForDebug;
          dbg->nearestTrafficTargetSpeed = desiredSpeed;
          dbg->nearestTrafficLaneId = agent.laneId;
          dbg->nearestTrafficLaneS = agent.laneS;
          dbg->nearestTrafficInput = world.has<VehicleInput>(e) ? *world.get<VehicleInput>(e) : VehicleInput{};
          dbg->nearestTrafficBodyActive = (state->physics && tv.body.valid()) ? state->physics->isBodyActive(tv.body) : false;
          if (TrafficSensors* sensors = world.get<TrafficSensors>(e))
          {
            dbg->nearestTrafficSensorHitDistance = sensors->lastHitDistance;
            dbg->nearestTrafficSensorHitType = sensors->lastHitType;
          }
        }
      }

      if (state->streaming)
      {
        const Vec3 pos{ tr.localPos[0], tr.localPos[1], tr.localPos[2] };
        const SectorCoord coord = state->streaming->partition.worldToSector(pos);
        updateSectorForEntity(world, state->streaming, e, coord);
      }
    });
  }

  void TrafficPhysicsSyncSystem(World& world, float dt, void* user)
  {
    (void)dt;
    TrafficPhysicsSyncState* state = static_cast<TrafficPhysicsSyncState*>(user);
    if (!state || !state->physics)
      return;

    TrafficDebugState* dbg = state->debug;
    Entity watch = dbg ? dbg->nearestTrafficEntity : kInvalidEntity;
    if (dbg && dbg->nearestTrafficDesyncEntity != watch)
    {
      dbg->nearestTrafficDesyncEntity = watch;
      dbg->nearestTrafficDesyncTimer = 0.0f;
      dbg->nearestTrafficDesyncLogged = 0;
    }

    world.ForEach<TrafficVehicle, Transform>([&](Entity e, TrafficVehicle& tv, Transform& tr)
    {
      if (tv.mode != TrafficSimMode::Physics)
        return;

      PhysicsBodyHandle* hb = world.get<PhysicsBodyHandle>(e);
      if (!hb || !hb->valid())
        return;

      float physPos[3]{};
      float physRot[3]{};
      if (!state->physics->getBodyTransform(*hb, physPos, physRot))
        return;

      const float ecsPos[3] = { tr.localPos[0], tr.localPos[1], tr.localPos[2] };

      float renderPosB[3] = { physPos[0], physPos[1], physPos[2] };
      if (VehicleComponent* vc = world.get<VehicleComponent>(e))
      {
        const float off[3] = { vc->centerOfMassOffset[0], vc->centerOfMassOffset[1], vc->centerOfMassOffset[2] };
        if (std::fabs(off[0]) > 1e-5f || std::fabs(off[1]) > 1e-5f || std::fabs(off[2]) > 1e-5f)
        {
          float rotOff[3]{};
          rotateVecEuler(physRot, off, rotOff);
          renderPosB[0] -= rotOff[0];
          renderPosB[1] -= rotOff[1];
          renderPosB[2] -= rotOff[2];
        }
      }

      const float* renderPos = renderPosB;

      tr.localPos[0] = renderPos[0];
      tr.localPos[1] = renderPos[1];
      tr.localPos[2] = renderPos[2];
      tr.localRot[0] = physRot[0];
      tr.localRot[1] = physRot[1];
      tr.localRot[2] = physRot[2];
      tr.dirty = true;

      if (dbg && e == watch)
      {
        const float dx = ecsPos[0] - renderPos[0];
        const float dy = ecsPos[1] - renderPos[1];
        const float dz = ecsPos[2] - renderPos[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

        dbg->nearestTrafficEcsPos[0] = ecsPos[0];
        dbg->nearestTrafficEcsPos[1] = ecsPos[1];
        dbg->nearestTrafficEcsPos[2] = ecsPos[2];
        dbg->nearestTrafficPhysPos[0] = physPos[0];
        dbg->nearestTrafficPhysPos[1] = physPos[1];
        dbg->nearestTrafficPhysPos[2] = physPos[2];
        dbg->nearestTrafficDesync = dist;

        if (dist > 0.5f)
          dbg->nearestTrafficDesyncTimer += dt;
        else
        {
          dbg->nearestTrafficDesyncTimer = 0.0f;
          dbg->nearestTrafficDesyncLogged = 0;
        }

        if (dbg->nearestTrafficDesyncTimer > 0.5f && dbg->nearestTrafficDesyncLogged == 0)
        {
          dbg->nearestTrafficDesyncLogged = 1;
          sc::log(sc::LogLevel::Warn,
                  "VISUAL/PHYSICS DESYNC e=%u dist=%.2f ecs=(%.2f,%.2f,%.2f) render=(%.2f,%.2f,%.2f) phys=(%.2f,%.2f,%.2f)",
                  e.index(),
                  dist,
                  ecsPos[0], ecsPos[1], ecsPos[2],
                  renderPos[0], renderPos[1], renderPos[2],
                  physPos[0], physPos[1], physPos[2]);
        }
      }
    });
  }

  void TrafficDebugDrawSystem(World& world, float dt, void* user)
  {
    (void)dt;
    TrafficDebugDrawState* state = static_cast<TrafficDebugDrawState*>(user);
    if (!state || !state->draw || !state->debug)
      return;

    TrafficDebugState& dbg = *state->debug;

    if (dbg.showLanes && state->lanes)
      state->lanes->debugDrawLanes(*state->draw, true);

    if (!dbg.showAgentTargets && !dbg.showSensorRays && !dbg.showTierColors)
      return;

    const float targetColor[3] = { 0.9f, 0.8f, 0.2f };
    const float rayHitColor[3] = { 1.0f, 0.2f, 0.2f };
    const float rayMissColor[3] = { 0.2f, 1.0f, 0.3f };
    const float tierAColor[3] = { 0.95f, 0.25f, 0.2f };
    const float tierBColor[3] = { 0.2f, 0.9f, 0.4f };
    const float tierCColor[3] = { 0.25f, 0.45f, 1.0f };

    world.ForEach<TrafficAgent, TrafficVehicle, Transform>([&](Entity e, TrafficAgent& agent, TrafficVehicle& tv, Transform& tr)
    {
      if (dbg.showAgentTargets && state->lanes && agent.laneId != kInvalidLaneId)
      {
        float target[3]{};
        if (state->lanes->getLookAheadPoint(agent.laneId, agent.laneS, agent.lookAheadDist, target))
        {
          const float p0[3] = { tr.localPos[0], tr.localPos[1], tr.localPos[2] };
          state->draw->addLine(p0, target, targetColor);
        }
      }

      if (dbg.showSensorRays)
      {
        const float yaw = tr.localRot[1];
        float forward[3] = { std::sin(yaw), 0.0f, std::cos(yaw) };
        normalize3(forward);

        const float rayLen = world.has<TrafficSensors>(e) ? world.get<TrafficSensors>(e)->frontRayLength : dbg.frontRayLength;
        const float origin[3] = {
          tr.localPos[0] + forward[0] * 1.5f,
          tr.localPos[1] + 0.4f,
          tr.localPos[2] + forward[2] * 1.5f
        };

        float end[3] = {
          origin[0] + forward[0] * rayLen,
          origin[1] + forward[1] * rayLen,
          origin[2] + forward[2] * rayLen
        };

        bool hit = false;
        if (state->physics)
        {
          RaycastHit rh = state->physics->raycast(origin, forward, rayLen, 0xFFFFFFFFu);
          if (rh.hit && rh.entity != e)
          {
            hit = true;
            end[0] = origin[0] + forward[0] * rh.distance;
            end[1] = origin[1] + forward[1] * rh.distance;
            end[2] = origin[2] + forward[2] * rh.distance;
          }
        }

        state->draw->addLine(origin, end, hit ? rayHitColor : rayMissColor);
      }

      if (dbg.showTierColors)
      {
        const float* color = tierCColor;
        if (tv.mode == TrafficSimMode::Physics) color = tierAColor;
        else if (tv.mode == TrafficSimMode::Kinematic) color = tierBColor;

        const float p0[3] = { tr.localPos[0], tr.localPos[1], tr.localPos[2] };
        const float p1[3] = { tr.localPos[0], tr.localPos[1] + 1.0f, tr.localPos[2] };
        state->draw->addLine(p0, p1, color);
      }
    });
  }
}
