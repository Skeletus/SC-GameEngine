#include "sc_app.h"
#include "sc_log.h"
#include "sc_vk.h"
#include "sc_jobs.h"
#include "sc_memtrack.h"
#include "sc_ecs.h"
#include "sc_scheduler.h"
#include "sc_debug_draw.h"
#include "sc_world_partition.h"

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
  worldStreaming.budgets.activeRadiusSectors = 2u;
  worldStreaming.budgets.maxActiveSectors = 25u;
  worldStreaming.budgets.maxEntitiesBudget = 5000u;
  worldStreaming.budgets.maxDrawsBudget = 6000u;

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

  sc::CameraSystemState cameraState{};
  cameraState.frame = &world.renderFrame();

  sc::DebugDraw debugDraw{};
  debugDraw.reserve(65536);

  sc::DebugDrawSystemState debugDrawState{};
  debugDrawState.draw = &debugDraw;
  debugDrawState.streaming = &worldStreaming;
  debugDrawState.culling = &culling;

  scheduler.addSystem("Spawner", sc::SystemPhase::Simulation, sc::SpawnerSystem, &spawner);
  scheduler.addSystem("WorldStreaming", sc::SystemPhase::Simulation, sc::WorldStreamingSystem, &worldStreaming, { "Spawner" });
  scheduler.addSystem("Transform", sc::SystemPhase::Simulation, sc::TransformSystem, nullptr, { "WorldStreaming" });
  scheduler.addSystem("Camera", sc::SystemPhase::Simulation, sc::CameraSystem, &cameraState, { "Transform" });
  scheduler.addSystem("Culling", sc::SystemPhase::RenderPrep, sc::CullingSystem, &culling, { "Camera" });
  scheduler.addSystem("RenderPrep", sc::SystemPhase::RenderPrep, sc::RenderPrepStreamingSystem, &renderPrep, { "Culling" });
  scheduler.addSystem("DebugDraw", sc::SystemPhase::RenderPrep, sc::DebugDrawSystem, &debugDrawState, { "RenderPrep" });
  scheduler.addSystem("Debug", sc::SystemPhase::Render, sc::DebugSystem, nullptr, { "DebugDraw" });
  scheduler.finalize();

  sc::Tick lastTicks = sc::nowTicks();

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
    scheduler.tick(world, dt);
    jobs.publishFrameTelemetry();

    vk.setTelemetry(jobs.getTelemetrySnapshot(), sc::memtrack_snapshot());
    vk.setEcsStats(world.statsSnapshot(), scheduler.statsSnapshot());
    vk.setRenderFrame(&world.renderFrame());
    vk.setDebugWorld(&world, spawner.camera, spawner.triangle, spawner.cube, spawner.root);
    vk.setWorldStreamingContext(&worldStreaming, &culling, &renderPrep);
    vk.setDebugDraw(&debugDraw);

    if (vk.beginFrame())
      vk.endFrame();
  }

  jobs.shutdown();
  vk.shutdown();
  app.shutdown();
  return 0;
}
