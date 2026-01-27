#include "sc_app.h"
#include "sc_log.h"
#include "sc_vk.h"

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
#if defined(SC_DEBUG)
  vkc.enableValidation = true;
#else
  vkc.enableValidation = false;
#endif

  if (!vk.init(app.window(), vkc))
  {
    sc::log(sc::LogLevel::Error, "Vulkan init failed.");
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

    if (vk.beginFrame())
      vk.endFrame();
  }

  vk.shutdown();
  app.shutdown();
  return 0;
}
