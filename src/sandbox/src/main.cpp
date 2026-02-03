#include "sc_app.h"
#include "sc_log.h"
#include "sc_vk.h"
#include "sc_jobs.h"
#include "sc_memtrack.h"
#include "sc_ecs.h"
#include "sc_scheduler.h"

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
  world.reserveEntities(1024);
  world.renderQueue().reserve(1024);

  sc::Scheduler scheduler;

  sc::SpawnerState spawner{};
  spawner.spawnCount = 256;
  spawner.churnEvery = 120;
  spawner.churnCount = 8;

  sc::RenderPrepState renderPrep{};
  renderPrep.queue = &world.renderQueue();

  scheduler.addSystem("Spawner", sc::SystemPhase::Simulation, sc::SpawnerSystem, &spawner);
  scheduler.addSystem("Transform", sc::SystemPhase::Simulation, sc::TransformSystem, nullptr, { "Spawner" });
  scheduler.addSystem("RenderPrep", sc::SystemPhase::RenderPrep, sc::RenderPrepSystem, &renderPrep, { "Transform" });
  scheduler.addSystem("Debug", sc::SystemPhase::Render, sc::DebugSystem, nullptr, { "RenderPrep" });
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

    scheduler.tick(world, dt);
    jobs.publishFrameTelemetry();

    vk.setTelemetry(jobs.getTelemetrySnapshot(), sc::memtrack_snapshot());
    vk.setEcsStats(world.statsSnapshot(), scheduler.statsSnapshot());
    vk.setDrawList(&world.renderQueue());

    if (vk.beginFrame())
      vk.endFrame();
  }

  jobs.shutdown();
  vk.shutdown();
  app.shutdown();
  return 0;
}
