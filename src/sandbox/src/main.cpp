#include "sc_app.h"
#include "sc_log.h"
#include <SDL.h>

int main()
{
  sc::App app;
  sc::AppConfig cfg;
  cfg.title = "sc_sandbox (SDL2 bring-up)";

  if (!app.init(cfg))
  {
    sc::log(sc::LogLevel::Error, "App init failed.");
    return 1;
  }

  while (app.pump())
  {
    // placeholder: here will go render + jobs + ECS later
    SDL_Delay(1); // avoid pegging CPU 100% for now
  }

  app.shutdown();
  return 0;
}
