#include "sc_app.h"
#include "sc_log.h"
#include "sc_vk.h"
#include "sc_jobs.h"
#include "sc_memtrack.h"
#include "sc_ecs.h"
#include "sc_scheduler.h"
#include "sc_debug_draw.h"
#include "sc_world_partition.h"
#include "sc_physics.h"
#include "sc_vehicle.h"
#include "sc_traffic_lanes.h"
#include "sc_traffic_spawner.h"
#include "sc_traffic_lod.h"
#include "sc_traffic_ai.h"

#include <SDL.h>
#include <thread>

static void handle_event(const SDL_Event& e, void* user)
{
  auto* vk = static_cast<sc::VkRenderer*>(user);
  if (vk) vk->onSDLEvent(e);
}

int main()
{
  sc::App app;
  sc::AppConfig cfg;
  cfg.title = "sc_sandbox (Vulkan bring-up)";

  if (!app.init(cfg))
  {
    sc::log(sc::LogLevel::Error, "App init failed.");
    return 1;
  }

  sc::VkRenderer vk;
  sc::VkConfig vkc;
  vkc.enableDebugUI = true;

  if (!vk.init(app.window(), vkc))
  {
    sc::log(sc::LogLevel::Error, "Vulkan init failed.");
    app.shutdown();
    return 1;
  }

  app.setEventCallback(handle_event, &vk);

  sc::JobSystem& jobs = sc::jobs();
  const uint32_t hw = std::thread::hardware_concurrency();
  const uint32_t workers = hw > 1 ? (hw - 1) : 1;
  if (!jobs.init(workers))
  {
    sc::log(sc::LogLevel::Error, "JobSystem init failed.");
    vk.shutdown();
    app.shutdown();
    return 1;
  }

  sc::World world;
  world.reserveEntities(16384);
  world.renderFrame().reserve(8192);

  sc::Scheduler scheduler;

  sc::SpawnerState spawner{};
  spawner.spawnCount = 0;
  spawner.churnEvery = 0;
  spawner.churnCount = 0;

  sc::WorldStreamingState worldStreaming{};
  sc::WorldPartitionConfig worldCfg{};
  worldCfg.sectorSizeMeters = 64.0f;
  worldCfg.seed = 424242u;
  worldCfg.propsPerSectorMin = 18u;
  worldCfg.propsPerSectorMax = 34u;
  worldCfg.includeGroundPlane = true;
  worldStreaming.partition.configure(worldCfg);

  const float sectorSize = worldStreaming.partition.config().sectorSizeMeters;
  spawner.overrideCamera = true;
  spawner.cameraPos[0] = sectorSize * 0.5f;
  spawner.cameraPos[1] = 6.0f;
  spawner.cameraPos[2] = sectorSize * 0.5f + 12.0f;
  spawner.cameraRot[0] = 0.0f;
  spawner.cameraRot[1] = 3.14159265f;
  spawner.cameraRot[2] = 0.0f;
  worldStreaming.budgets.loadRadiusSectors = 2u;
  worldStreaming.budgets.unloadRadiusSectors = 3u;
  worldStreaming.budgets.maxActiveSectors = 25u;
  worldStreaming.budgets.maxEntitiesBudget = 5000u;
  worldStreaming.budgets.maxDrawsBudget = 6000u;
  worldStreaming.budgets.maxConcurrentLoads = 4u;
  worldStreaming.budgets.maxActivationsPerFrame = 2u;
  worldStreaming.budgets.maxDespawnsPerFrame = 128u;

  sc::CullingState culling{};
  culling.frame = &world.renderFrame();
  culling.candidates.reserve(8192);
  culling.visible.reserve(8192);
  culling.culled.reserve(8192);
  culling.visibilityMask.reserve(8192);

  sc::RenderPrepStreamingState renderPrep{};
  renderPrep.frame = &world.renderFrame();
  renderPrep.culling = &culling;
  renderPrep.streaming = &worldStreaming;
  renderPrep.assets = &vk.assets();

  sc::CameraSystemState cameraState{};
  cameraState.frame = &world.renderFrame();

  sc::DebugDraw debugDraw{};
  debugDraw.reserve(65536);

  sc::DebugDrawSystemState debugDrawState{};
  debugDrawState.draw = &debugDraw;
  debugDrawState.streaming = &worldStreaming;
  debugDrawState.culling = &culling;

  sc::PhysicsWorld physicsWorld{};
  physicsWorld.init();

  sc::PhysicsDebugState physicsDebug{};

  sc::PhysicsSyncState physicsSync{};
  physicsSync.world = &physicsWorld;
  physicsSync.debug = &physicsDebug;
  physicsSync.tracked.reserve(4096);

  sc::VehicleDebugState vehicleDebug{};
  vehicleDebug.cameraEnabled = true;

  sc::VehicleSystemState vehicleSystem{};
  vehicleSystem.world = &physicsWorld;
  vehicleSystem.sync = &physicsSync;
  vehicleSystem.debug = &vehicleDebug;
  vehicleSystem.tracked.reserve(32);

  sc::VehicleInputState vehicleInput{};
  vehicleInput.debug = &vehicleDebug;

  sc::VehicleCameraState vehicleCamera{};
  vehicleCamera.world = &physicsWorld;
  vehicleCamera.debug = &vehicleDebug;
  vehicleCamera.useFixedFollow = true;
  vehicleCamera.fixedOffset[0] = 0.0f;
  vehicleCamera.fixedOffset[1] = 3.0f;
  vehicleCamera.fixedOffset[2] = -8.0f;
  vehicleCamera.fixedRot[0] = -0.42f;
  vehicleCamera.fixedRot[1] = 3.0f;
  vehicleCamera.fixedRot[2] = 0.0f;
  vehicleCamera.dynamicFov = true;
  vehicleCamera.minFov = 60.0f;
  vehicleCamera.maxFov = 75.0f;

  sc::VehicleDemoState vehicleDemo{};
  vehicleDemo.debug = &vehicleDebug;

  sc::VehicleStreamingPinState vehiclePins{};
  vehiclePins.streaming = &worldStreaming;
  vehiclePins.debug = &vehicleDebug;
  vehiclePins.pinRadius = 1u;

  sc::VehicleDebugDrawState vehicleDraw{};
  vehicleDraw.draw = &debugDraw;
  vehicleDraw.debug = &vehicleDebug;

  sc::PhysicsDebugDrawState physicsDraw{};
  physicsDraw.world = &physicsWorld;
  physicsDraw.debug = &physicsDebug;
  physicsDraw.draw = &debugDraw;

  sc::TrafficLaneGraph trafficLanes{};
  sc::TrafficDebugState trafficDebug{};

  sc::TrafficSpawnerState trafficSpawner{};
  trafficSpawner.streaming = &worldStreaming;
  trafficSpawner.lanes = &trafficLanes;
  trafficSpawner.debug = &trafficDebug;
  trafficSpawner.vehicleDebug = &vehicleDebug;
  trafficSpawner.meshId = 1u;

  sc::TrafficLODState trafficLod{};
  trafficLod.streaming = &worldStreaming;
  trafficLod.lanes = &trafficLanes;
  trafficLod.debug = &trafficDebug;
  trafficLod.vehicleDebug = &vehicleDebug;
  trafficLod.meshId = 1u;

  sc::TrafficAIState trafficAI{};
  trafficAI.lanes = &trafficLanes;
  trafficAI.debug = &trafficDebug;
  trafficAI.physics = &physicsWorld;
  trafficAI.streaming = &worldStreaming;

  sc::TrafficPhysicsSyncState trafficPhysSync{};
  trafficPhysSync.physics = &physicsWorld;
  trafficPhysSync.debug = &trafficDebug;

  sc::TrafficDebugDrawState trafficDrawState{};
  trafficDrawState.lanes = &trafficLanes;
  trafficDrawState.debug = &trafficDebug;
  trafficDrawState.draw = &debugDraw;
  trafficDrawState.physics = &physicsWorld;

  sc::TrafficPinState trafficPins{};
  trafficPins.streaming = &worldStreaming;
  trafficPins.debug = &trafficDebug;

  sc::PhysicsDemoState physicsDemo{};
  physicsDemo.debug = &physicsDebug;
  {
    physicsDemo.basePos[0] = sectorSize * 0.5f;
    physicsDemo.basePos[1] = 6.0f;
    physicsDemo.basePos[2] = sectorSize * 0.5f;
  }

  const sc::TextureHandle checkerTex = vk.assets().loadTexture2D("textures/checker.ppm");
  sc::MaterialDesc checkerDesc{};
  checkerDesc.albedo = checkerTex;
  checkerDesc.unlit = false;
  checkerDesc.transparent = false;
  const sc::MaterialHandle checkerMat = vk.assets().createMaterial(checkerDesc);
  physicsDemo.materialId = (checkerMat != sc::kInvalidMaterialHandle) ? checkerMat : 0u;

  vehicleDemo.materialId = physicsDemo.materialId;
  trafficSpawner.materialId = vehicleDemo.materialId;
  trafficLod.materialId = vehicleDemo.materialId;
  vehicleDemo.spawnPos[0] = sectorSize * 0.5f;
  vehicleDemo.spawnPos[1] = 2.0f;
  vehicleDemo.spawnPos[2] = sectorSize * 0.5f;
  vehicleDemo.spawnRot[0] = 0.0f;
  vehicleDemo.spawnRot[1] = 0.0f;
  vehicleDemo.spawnRot[2] = 0.0f;

  scheduler.addSystem("VehicleInput", sc::SystemPhase::Input, sc::VehicleInputSystem, &vehicleInput);
  scheduler.addSystem("Spawner", sc::SystemPhase::Simulation, sc::SpawnerSystem, &spawner);
  scheduler.addSystem("VehicleDemo", sc::SystemPhase::Simulation, sc::VehicleDemoSystem, &vehicleDemo, { "Spawner" });
  scheduler.addSystem("VehicleStreamingPin", sc::SystemPhase::Simulation, sc::VehicleStreamingPinSystem, &vehiclePins, { "VehicleDemo" });
  scheduler.addSystem("TrafficPin", sc::SystemPhase::Simulation, sc::TrafficPinSystem, &trafficPins, { "VehicleStreamingPin" });
  scheduler.addSystem("WorldStreaming", sc::SystemPhase::Simulation, sc::WorldStreamingSystem, &worldStreaming, { "Spawner", "TrafficPin" });
  scheduler.addSystem("TrafficSpawner", sc::SystemPhase::Simulation, sc::TrafficSpawnerSystem, &trafficSpawner, { "WorldStreaming" });
  scheduler.addSystem("TrafficLOD", sc::SystemPhase::Simulation, sc::TrafficLODSystem, &trafficLod, { "TrafficSpawner" });
  scheduler.addSystem("PhysicsDemo", sc::SystemPhase::Simulation, sc::PhysicsDemoSystem, &physicsDemo, { "WorldStreaming" });
  scheduler.addSystem("TrafficAI", sc::SystemPhase::FixedUpdate, sc::TrafficAISystem, &trafficAI, { "TrafficLOD" });
  scheduler.addSystem("VehiclePreStep", sc::SystemPhase::FixedUpdate, sc::VehicleSystemPreStep, &vehicleSystem, { "PhysicsDemo", "TrafficAI" });
  scheduler.addSystem("PhysicsSync", sc::SystemPhase::FixedUpdate, sc::PhysicsSyncSystem, &physicsSync, { "PhysicsDemo", "VehiclePreStep" });
  scheduler.addSystem("TrafficPhysicsSync", sc::SystemPhase::FixedUpdate, sc::TrafficPhysicsSyncSystem, &trafficPhysSync, { "PhysicsSync" });
  scheduler.addSystem("VehiclePostStep", sc::SystemPhase::FixedUpdate, sc::VehicleSystemPostStep, &vehicleSystem, { "TrafficPhysicsSync" });
  scheduler.addSystem("VehicleCamera", sc::SystemPhase::RenderPrep, sc::VehicleCameraSystem, &vehicleCamera);
  scheduler.addSystem("Transform", sc::SystemPhase::RenderPrep, sc::TransformSystem, nullptr, { "PhysicsDemo", "VehicleCamera" });
  scheduler.addSystem("Camera", sc::SystemPhase::RenderPrep, sc::CameraSystem, &cameraState, { "Transform" });
  scheduler.addSystem("Culling", sc::SystemPhase::RenderPrep, sc::CullingSystem, &culling, { "Camera" });
  scheduler.addSystem("RenderPrep", sc::SystemPhase::RenderPrep, sc::RenderPrepStreamingSystem, &renderPrep, { "Culling" });
  scheduler.addSystem("DebugDraw", sc::SystemPhase::RenderPrep, sc::DebugDrawSystem, &debugDrawState, { "RenderPrep" });
  scheduler.addSystem("TrafficDebugDraw", sc::SystemPhase::RenderPrep, sc::TrafficDebugDrawSystem, &trafficDrawState, { "DebugDraw" });
  scheduler.addSystem("VehicleDebugDraw", sc::SystemPhase::RenderPrep, sc::VehicleDebugDrawSystem, &vehicleDraw, { "TrafficDebugDraw" });
  scheduler.addSystem("PhysicsDebugDraw", sc::SystemPhase::RenderPrep, sc::PhysicsDebugDrawSystem, &physicsDraw, { "VehicleDebugDraw" });
  scheduler.addSystem("Debug", sc::SystemPhase::Render, sc::DebugSystem, nullptr, { "DebugDraw" });
  scheduler.finalize();

  sc::Tick lastTicks = sc::nowTicks();
  float fixedAccumulator = 0.0f;
  const float fixedDt = 1.0f / 60.0f;
  const uint32_t maxFixedSteps = 4;

  while (app.pump())
  {
    if (app.wasResized())
    {
      vk.onResizeRequest();
      app.clearResized();
    }

    jobs.beginFrame();

    const sc::Tick now = sc::nowTicks();
    const float dt = (float)sc::ticksToSeconds(now - lastTicks);
    lastTicks = now;

    cameraState.aspect = vk.swapchainAspect();
    float fixedStepDt = fixedDt;
    uint32_t fixedSteps = 0;

    if (!physicsDebug.pausePhysics)
    {
      fixedAccumulator += dt;
      const float maxAccum = fixedDt * (float)maxFixedSteps;
      if (fixedAccumulator > maxAccum)
        fixedAccumulator = maxAccum;

      while (fixedAccumulator >= fixedDt && fixedSteps < maxFixedSteps)
      {
        fixedAccumulator -= fixedDt;
        fixedSteps++;
      }
    }
    else
    {
      fixedAccumulator = 0.0f;
      fixedSteps = 1;
      fixedStepDt = 0.0f;
    }

    scheduler.tick(world, dt, fixedSteps, fixedStepDt);
    jobs.publishFrameTelemetry();

    vk.setTelemetry(jobs.getTelemetrySnapshot(), sc::memtrack_snapshot());
    vk.setEcsStats(world.statsSnapshot(), scheduler.statsSnapshot());
    vk.setRenderFrame(&world.renderFrame());
    vk.setDebugWorld(&world, spawner.camera, spawner.triangle, spawner.cube, spawner.root);
    vk.setWorldStreamingContext(&worldStreaming, &culling, &renderPrep);
    vk.setDebugDraw(&debugDraw);
    vk.setPhysicsContext(&physicsDebug);
    vk.setVehicleContext(&vehicleDebug);
    vk.setTrafficContext(&trafficDebug);

    if (vk.beginFrame())
      vk.endFrame();
  }

  worldStreaming.partition.shutdownStreaming();
  physicsWorld.shutdown();
  jobs.shutdown();
  vk.shutdown();
  app.shutdown();
  return 0;
}
