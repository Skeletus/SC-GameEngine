#include "sc_vehicle.h"
#include "sc_world_partition.h"
#include "sc_debug_draw.h"
#include "sc_math.h"

#include <SDL.h>

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

    static float lerp(float a, float b, float t)
    {
      return a + (b - a) * t;
    }

    static float length3(const float v[3])
    {
      return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
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

    static float smoothExp(float current, float target, float response, float dt)
    {
      const float t = 1.0f - std::exp(-response * dt);
      return current + (target - current) * t;
    }

    static void rotateVecEuler(const float rot[3], const float v[3], float out[3])
    {
      const Mat4 r = mat4_rotation_xyz(rot[0], rot[1], rot[2]);
      out[0] = r.m[0] * v[0] + r.m[4] * v[1] + r.m[8] * v[2];
      out[1] = r.m[1] * v[0] + r.m[5] * v[1] + r.m[9] * v[2];
      out[2] = r.m[2] * v[0] + r.m[6] * v[1] + r.m[10] * v[2];
    }

    static Entity pickActiveCamera(World& world, Transform*& outTransform, Camera*& outCamera)
    {
      Entity active = kInvalidEntity;
      Entity fallback = kInvalidEntity;
      Transform* activeTransform = nullptr;
      Camera* activeCam = nullptr;
      Transform* fallbackTransform = nullptr;
      Camera* fallbackCam = nullptr;

      world.ForEach<Camera, Transform>([&](Entity e, Camera& cam, Transform& tr)
      {
        if (!fallbackCam)
        {
          fallback = e;
          fallbackCam = &cam;
          fallbackTransform = &tr;
        }
        if (!activeCam && cam.active)
        {
          active = e;
          activeCam = &cam;
          activeTransform = &tr;
        }
      });

      if (activeCam)
      {
        outTransform = activeTransform;
        outCamera = activeCam;
        return active;
      }
      outTransform = fallbackTransform;
      outCamera = fallbackCam;
      return fallback;
    }

    static Entity pickActiveVehicle(World& world, VehicleDebugState* debug)
    {
      if (debug && isValidEntity(debug->activeVehicle) &&
          world.isAlive(debug->activeVehicle) &&
          world.has<VehicleRuntime>(debug->activeVehicle))
      {
        return debug->activeVehicle;
      }

      Entity active = kInvalidEntity;
      world.ForEach<VehicleRuntime>([&](Entity e, VehicleRuntime&)
      {
        if (!isValidEntity(active))
          active = e;
      });

      if (debug)
        debug->activeVehicle = active;
      return active;
    }
  }

  void VehicleInputSystem(World& world, float dt, void* user)
  {
    (void)dt;
    VehicleInputState* state = static_cast<VehicleInputState*>(user);
    VehicleDebugState* debug = state ? state->debug : nullptr;

    Entity active = pickActiveVehicle(world, debug);

    world.ForEach<VehicleInput>([&](Entity, VehicleInput& in)
    {
      in = {};
    });

    if (!isValidEntity(active))
    {
      world.ForEach<VehicleInput, VehicleComponent>([&](Entity e, VehicleInput&, VehicleComponent&)
      {
        if (!isValidEntity(active))
          active = e;
      });
      if (debug)
        debug->activeVehicle = active;
    }

    if (!isValidEntity(active))
      return;

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const float throttle = keys[SDL_SCANCODE_W] ? 1.0f : 0.0f;
    const float brake = keys[SDL_SCANCODE_S] ? 1.0f : 0.0f;
    float steer = 0.0f;
    if (keys[SDL_SCANCODE_A]) steer += 1.0f;
    if (keys[SDL_SCANCODE_D]) steer -= 1.0f;
    const float handbrake = keys[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f;

    if (VehicleInput* in = world.get<VehicleInput>(active))
    {
      in->throttle = throttle;
      in->brake = brake;
      in->steer = steer;
      in->handbrake = handbrake;
    }

    if (debug)
      debug->activeVehicle = active;
  }

  void VehicleSystemPreStep(World& world, float dt, void* user)
  {
    VehicleSystemState* state = static_cast<VehicleSystemState*>(user);
    if (!state || !state->world || !state->sync)
      return;

    PhysicsWorld& physics = *state->world;

    for (size_t i = 0; i < state->tracked.size();)
    {
      const VehicleTracked vt = state->tracked[i];
      const bool alive = world.isAlive(vt.entity);
      const bool has = alive &&
                       world.has<VehicleComponent>(vt.entity) &&
                       world.has<VehicleInput>(vt.entity) &&
                       world.has<Transform>(vt.entity) &&
                       world.has<RigidBody>(vt.entity) &&
                       world.has<Collider>(vt.entity) &&
                       world.has<PhysicsBodyHandle>(vt.entity);

      if (!alive || !has || !vt.handle.valid())
      {
        physics.removeRaycastVehicle(vt.handle);
        if (alive)
          world.remove<VehicleRuntime>(vt.entity);
        state->tracked[i] = state->tracked.back();
        state->tracked.pop_back();
        continue;
      }
      ++i;
    }

    world.ForEach<VehicleComponent, VehicleInput, Transform, RigidBody, Collider>([&](Entity e,
                                                                                     VehicleComponent& vc,
                                                                                     VehicleInput& vin,
                                                                                     Transform& tr,
                                                                                     RigidBody& rb,
                                                                                     Collider& col)
    {
      (void)vin;
      if (world.has<VehicleRuntime>(e))
        return;

      rb.type = RigidBodyType::Dynamic;
      rb.mass = vc.mass;

      PhysicsBodyHandle body{};
      if (world.has<PhysicsBodyHandle>(e))
      {
        PhysicsBodyHandle* existing = world.get<PhysicsBodyHandle>(e);
        if (existing)
          body = *existing;
      }

      if (!body.valid())
      {
        body = physics.addRigidBodyWithComOffset(e, tr, rb, col, vc.centerOfMassOffset);
        if (!body.valid())
          return;

        PhysicsBodyHandle& hb = world.add<PhysicsBodyHandle>(e);
        hb = body;

        bool alreadyTracked = false;
        for (const auto& tb : state->sync->tracked)
        {
          if (tb.entity == e)
          {
            alreadyTracked = true;
            break;
          }
        }
        if (!alreadyTracked)
          state->sync->tracked.push_back({ e, body });
      }

      const float sx = std::fabs(tr.localScale[0]) > 1e-6f ? std::fabs(tr.localScale[0]) : 1.0f;
      const float sy = std::fabs(tr.localScale[1]) > 1e-6f ? std::fabs(tr.localScale[1]) : 1.0f;
      const float sz = std::fabs(tr.localScale[2]) > 1e-6f ? std::fabs(tr.localScale[2]) : 1.0f;

      const float hx = std::max(0.2f, col.halfExtents[0] * sx);
      const float hy = std::max(0.2f, col.halfExtents[1] * sy);
      const float hz = std::max(0.4f, col.halfExtents[2] * sz);

      const float wheelX = hx - vc.wheelWidth * 0.5f;
      const float wheelY = -hy + vc.wheelRadius;
      const float wheelFrontZ = hz - vc.wheelRadius * 0.5f;
      const float wheelRearZ = -hz + vc.wheelRadius * 0.5f;

      VehicleWheelConfig wheels[4]{};
      const float com[3] = { vc.centerOfMassOffset[0], vc.centerOfMassOffset[1], vc.centerOfMassOffset[2] };

      auto setWheel = [&](uint32_t idx, float x, float y, float z, bool front)
      {
        wheels[idx].connectionPoint[0] = x - com[0];
        wheels[idx].connectionPoint[1] = y - com[1];
        wheels[idx].connectionPoint[2] = z - com[2];
        wheels[idx].direction[0] = 0.0f;
        wheels[idx].direction[1] = -1.0f;
        wheels[idx].direction[2] = 0.0f;
        wheels[idx].axle[0] = -1.0f;
        wheels[idx].axle[1] = 0.0f;
        wheels[idx].axle[2] = 0.0f;
        wheels[idx].suspensionRestLength = vc.suspensionRestLength;
        wheels[idx].wheelRadius = vc.wheelRadius;
        wheels[idx].wheelWidth = vc.wheelWidth;
        wheels[idx].front = front;
      };

      setWheel(0, -wheelX, wheelY, wheelFrontZ, true);
      setWheel(1,  wheelX, wheelY, wheelFrontZ, true);
      setWheel(2, -wheelX, wheelY, wheelRearZ, false);
      setWheel(3,  wheelX, wheelY, wheelRearZ, false);

      VehicleHandle vh = physics.createRaycastVehicle(body, vc, wheels, 4);
      if (!vh.valid())
        return;

      VehicleRuntime& rt = world.add<VehicleRuntime>(e);
      rt.handle = vh;
      rt.body = body;
      rt.wheelCount = 4u;

      state->tracked.push_back({ e, vh, body });

      if (state->debug && !isValidEntity(state->debug->activeVehicle))
        state->debug->activeVehicle = e;
    });

    world.ForEach<VehicleComponent, VehicleInput, VehicleRuntime, Transform, RigidBody>([&](Entity,
                                                                                           VehicleComponent& vc,
                                                                                           VehicleInput& input,
                                                                                           VehicleRuntime& rt,
                                                                                           Transform& tr,
                                                                                           RigidBody& rb)
    {
      (void)tr;
      if (!rt.handle.valid())
        return;

      rb.mass = vc.mass;

      physics.updateVehicleTuning(rt.handle, vc);

      const float throttleTarget = clamp01(input.throttle);
      const float brakeTarget = clamp01(input.brake);
      const float steerTarget = clamp(input.steer, -1.0f, 1.0f);
      const float handTarget = clamp01(input.handbrake);

      rt.throttle = smoothExp(rt.throttle, throttleTarget, 6.0f, dt);
      rt.brake = smoothExp(rt.brake, brakeTarget, 6.0f, dt);
      rt.steer = smoothExp(rt.steer, steerTarget, std::max(1.0f, vc.steerResponse), dt);
      rt.handbrake = smoothExp(rt.handbrake, handTarget, 10.0f, dt);

      float throttle = rt.throttle * rt.throttle;
      float brake = rt.brake * rt.brake;
      float handbrake = rt.handbrake * rt.handbrake;

      float steerLimit = vc.maxSteerAngle;
      if (vc.maxSpeed > 0.1f)
      {
        const float speedRatio = clamp(rt.speedMs / vc.maxSpeed, 0.0f, 1.0f);
        steerLimit = vc.maxSteerAngle * lerp(1.0f, 0.2f, speedRatio);
      }

      rt.steerAngle = rt.steer * steerLimit;
      rt.engineForce = throttle * vc.engineForce;
      rt.brakeForce = brake * vc.brakeForce;

      if (vc.maxSpeed > 0.1f && rt.speedMs > vc.maxSpeed)
        rt.engineForce = 0.0f;
      if (vc.maxSpeed > 0.1f && rt.speedMs > vc.maxSpeed * 1.05f)
        rt.brakeForce = std::max(rt.brakeForce, vc.brakeForce * 0.2f);

      const float hbForce = handbrake * vc.handbrakeForce;
      physics.setVehicleControls(rt.handle, rt.engineForce, rt.brakeForce, rt.steerAngle, hbForce);
    });
  }

  void VehicleSystemPostStep(World& world, float dt, void* user)
  {
    (void)dt;
    VehicleSystemState* state = static_cast<VehicleSystemState*>(user);
    if (!state || !state->world)
      return;

    PhysicsWorld& physics = *state->world;
    VehicleDebugState* debug = state->debug;

    world.ForEach<VehicleComponent, VehicleRuntime, Transform>([&](Entity e,
                                                                   VehicleComponent& vc,
                                                                   VehicleRuntime& rt,
                                                                   Transform& tr)
    {
      if (!rt.handle.valid())
        return;

      physics.getVehicleTelemetry(rt.handle, rt, vc.suspensionRestLength);

      const float off[3] = { vc.centerOfMassOffset[0], vc.centerOfMassOffset[1], vc.centerOfMassOffset[2] };
      if (std::fabs(off[0]) > 1e-5f || std::fabs(off[1]) > 1e-5f || std::fabs(off[2]) > 1e-5f)
      {
        float rotOff[3]{};
        rotateVecEuler(tr.localRot, off, rotOff);
        tr.localPos[0] -= rotOff[0];
        tr.localPos[1] -= rotOff[1];
        tr.localPos[2] -= rotOff[2];
        tr.dirty = true;
      }

      if (debug && debug->activeVehicle == e)
        debug->telemetry = rt;
    });
  }

  void VehicleDemoSystem(World& world, float dt, void* user)
  {
    (void)dt;
    VehicleDemoState* state = static_cast<VehicleDemoState*>(user);
    if (!state)
      return;

    const bool requestRespawn = state->debug ? state->debug->requestRespawn : false;
    if (state->initialized && !requestRespawn)
      return;

    if (state->debug)
      state->debug->requestRespawn = false;

    if (state->initialized && isValidEntity(state->vehicle))
      world.destroy(state->vehicle);

    state->vehicle = world.create();

    Transform& t = world.add<Transform>(state->vehicle);
    setLocal(t, state->spawnPos, state->spawnRot, state->spawnScale);

    RenderMesh& rm = world.add<RenderMesh>(state->vehicle);
    rm.meshId = state->meshId;
    rm.materialId = state->materialId;

    Collider& col = world.add<Collider>(state->vehicle);
    col.type = ColliderType::Box;
    col.halfExtents[0] = 0.5f;
    col.halfExtents[1] = 0.5f;
    col.halfExtents[2] = 0.5f;

    RigidBody& rb = world.add<RigidBody>(state->vehicle);
    rb.type = RigidBodyType::Dynamic;
    rb.mass = 1200.0f;
    rb.friction = 0.9f;
    rb.angularDamping = 0.2f;

    VehicleComponent& vc = world.add<VehicleComponent>(state->vehicle);
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
    vc.centerOfMassOffset[1] = -0.35f;
    vc.centerOfMassOffset[2] = 0.0f;

    world.add<VehicleInput>(state->vehicle);

    setName(world.add<Name>(state->vehicle), "Vehicle");

    if (state->debug)
      state->debug->activeVehicle = state->vehicle;

    state->initialized = true;
  }

  void VehicleStreamingPinSystem(World& world, float dt, void* user)
  {
    (void)dt;
    VehicleStreamingPinState* state = static_cast<VehicleStreamingPinState*>(user);
    if (!state || !state->streaming)
      return;

    state->streaming->pinnedCenters.clear();
    state->streaming->pinnedRadius = state->pinRadius;

    const Entity activeVehicle = pickActiveVehicle(world, state->debug);
    if (!isValidEntity(activeVehicle))
      return;

    const Transform* tr = world.get<Transform>(activeVehicle);
    if (!tr)
      return;

    const Vec3 pos{ tr->localPos[0], tr->localPos[1], tr->localPos[2] };
    const SectorCoord coord = state->streaming->partition.worldToSector(pos);
    state->streaming->pinnedCenters.push_back(coord);
  }

  void VehicleCameraSystem(World& world, float dt, void* user)
  {
    VehicleCameraState* state = static_cast<VehicleCameraState*>(user);
    if (!state)
      return;

    if (state->debug && !state->debug->cameraEnabled)
      return;

    const Entity vehicle = pickActiveVehicle(world, state->debug);
    if (!isValidEntity(vehicle))
      return;

    Transform* vehicleTr = world.get<Transform>(vehicle);
    VehicleRuntime* vehicleRt = world.get<VehicleRuntime>(vehicle);
    VehicleComponent* vehicleComp = world.get<VehicleComponent>(vehicle);
    if (!vehicleTr || !vehicleRt || !vehicleComp)
      return;

    Transform* camTr = nullptr;
    Camera* cam = nullptr;
    const Entity camEnt = pickActiveCamera(world, camTr, cam);
    if (!camTr || !cam || !isValidEntity(camEnt))
      return;

    if (state->useFixedFollow)
    {
      camTr->localPos[0] = vehicleTr->localPos[0] + state->fixedOffset[0];
      camTr->localPos[1] = vehicleTr->localPos[1] + state->fixedOffset[1];
      camTr->localPos[2] = vehicleTr->localPos[2] + state->fixedOffset[2];

      camTr->localRot[0] = state->fixedRot[0];
      camTr->localRot[1] = state->fixedRot[1];
      camTr->localRot[2] = state->fixedRot[2];
      camTr->dirty = true;

      state->velocity[0] = 0.0f;
      state->velocity[1] = 0.0f;
      state->velocity[2] = 0.0f;
      return;
    }

    float target[3] = { vehicleTr->localPos[0], vehicleTr->localPos[1], vehicleTr->localPos[2] };

    float forward[3]{};
    const float yaw = vehicleTr->localRot[1];
    forward[0] = std::sin(yaw);
    forward[1] = 0.0f;
    forward[2] = std::cos(yaw);
    normalize3(forward);
    float lookAt[3] = {
      target[0] - forward[0] * state->rearOffset,
      target[1],
      target[2] - forward[2] * state->rearOffset
    };

    const float pitchRad = state->lookDownDegrees * 3.1415926535f / 180.0f;
    const float angleHeight = std::tan(pitchRad) * state->distance;
    const float desiredHeight = angleHeight + state->height;

    float desired[3] = {
      lookAt[0] - forward[0] * state->distance,
      lookAt[1] + desiredHeight,
      lookAt[2] - forward[2] * state->distance
    };

    if (state->enableOcclusion && state->world)
    {
      float dir[3] = { desired[0] - lookAt[0], desired[1] - lookAt[1], desired[2] - lookAt[2] };
      const float dist = length3(dir);
      if (dist > 1e-3f)
      {
        RaycastHit hit = state->world->raycast(lookAt, dir, dist, 2u);
        if (hit.hit)
        {
          float n[3] = { dir[0], dir[1], dir[2] };
          normalize3(n);
          const float safeDist = std::max(0.0f, hit.distance - state->occlusionPadding);
          desired[0] = lookAt[0] + n[0] * safeDist;
          desired[1] = lookAt[1] + n[1] * safeDist;
          desired[2] = lookAt[2] + n[2] * safeDist;
        }
      }
    }

    float pos[3] = { camTr->localPos[0], camTr->localPos[1], camTr->localPos[2] };
    const float stiffness = state->positionStiffness;
    const float damping = state->positionDamping;

    float accel[3] = {
      (desired[0] - pos[0]) * stiffness - state->velocity[0] * damping,
      (desired[1] - pos[1]) * stiffness - state->velocity[1] * damping,
      (desired[2] - pos[2]) * stiffness - state->velocity[2] * damping
    };

    state->velocity[0] += accel[0] * dt;
    state->velocity[1] += accel[1] * dt;
    state->velocity[2] += accel[2] * dt;

    pos[0] += state->velocity[0] * dt;
    pos[1] += state->velocity[1] * dt;
    pos[2] += state->velocity[2] * dt;

    camTr->localPos[0] = pos[0];
    camTr->localPos[1] = pos[1];
    camTr->localPos[2] = pos[2];

    float dir[3] = { lookAt[0] - pos[0], lookAt[1] - pos[1], lookAt[2] - pos[2] };
    normalize3(dir);
    const float yawCam = std::atan2(dir[0], dir[2]);
    const float pitchCam = std::atan2(-dir[1], std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]));
    camTr->localRot[0] = pitchCam;
    camTr->localRot[1] = yawCam;
    camTr->localRot[2] = 0.0f;
    camTr->dirty = true;

    if (state->dynamicFov && vehicleComp && cam && vehicleComp->maxSpeed > 0.1f)
    {
      const float ratio = clamp(vehicleRt->speedMs / vehicleComp->maxSpeed, 0.0f, 1.0f);
      cam->fovY = lerp(state->minFov, state->maxFov, ratio);
    }
  }

  void VehicleDebugDrawSystem(World& world, float dt, void* user)
  {
    (void)dt;
    VehicleDebugDrawState* state = static_cast<VehicleDebugDrawState*>(user);
    if (!state || !state->draw || !state->debug)
      return;

    if (!state->debug->showWheelRaycasts && !state->debug->showContactPoints)
      return;

    const Entity active = pickActiveVehicle(world, state->debug);
    const bool onlyActive = isValidEntity(active);

    const float rayColor[3] = { 0.9f, 0.6f, 0.2f };
    const float hitColor[3] = { 0.2f, 0.9f, 0.3f };

    auto drawWheel = [&](const VehicleRuntime& rt, uint32_t i)
    {
      const float* wp = rt.wheelWorldPos[i];
      const float* cp = rt.wheelContactPoint[i];

      if (state->debug->showWheelRaycasts)
        state->draw->addLine(wp, cp, rayColor);

      if (state->debug->showContactPoints && rt.wheelContact[i])
      {
        const float cross = 0.2f;
        float px[3] = { cp[0] - cross, cp[1], cp[2] };
        float nx[3] = { cp[0] + cross, cp[1], cp[2] };
        float py[3] = { cp[0], cp[1] - cross, cp[2] };
        float ny[3] = { cp[0], cp[1] + cross, cp[2] };
        float pz[3] = { cp[0], cp[1], cp[2] - cross };
        float nz[3] = { cp[0], cp[1], cp[2] + cross };
        state->draw->addLine(px, nx, hitColor);
        state->draw->addLine(py, ny, hitColor);
        state->draw->addLine(pz, nz, hitColor);
      }
    };

    world.ForEach<VehicleRuntime>([&](Entity e, VehicleRuntime& rt)
    {
      if (onlyActive && e != active)
        return;
      const uint32_t count = std::min(rt.wheelCount, kMaxVehicleWheels);
      for (uint32_t i = 0; i < count; ++i)
        drawWheel(rt, i);
    });
  }
}
