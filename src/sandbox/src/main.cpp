#include "sc_app.h"
#include "sc_log.h"
#include "sc_vk.h"
#include "sc_jobs.h"
#include "sc_memtrack.h"

#include <SDL.h>
#include <thread>

static volatile uint32_t g_jobSink = 0;

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

  while (app.pump())
  {
    if (app.wasResized())
    {
      vk.onResizeRequest();
      app.clearResized();
    }

    jobs.beginFrame();
    auto handle = jobs.Dispatch(10000, 64, [](const sc::JobContext& ctx)
    {
      uint32_t acc = 0;
      for (uint32_t i = ctx.start; i < ctx.end; ++i)
        acc += (i * 1664525u + 1013904223u);
      g_jobSink += acc;
    });
    jobs.Wait(handle);
    jobs.publishFrameTelemetry();

    vk.setTelemetry(jobs.getTelemetrySnapshot(), sc::memtrack_snapshot());

    if (vk.beginFrame())
      vk.endFrame();
  }

  jobs.shutdown();
  vk.shutdown();
  app.shutdown();
  return 0;
}
