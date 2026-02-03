#include "sc_app.h"
#include "sc_log.h"

#include <SDL.h>

namespace sc
{
  static uint64_t now_counter() { return SDL_GetPerformanceCounter(); }
  static double   now_freq()    { return static_cast<double>(SDL_GetPerformanceFrequency()); }

  bool App::init(const AppConfig& cfg)
  {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
    {
      sc::log(sc::LogLevel::Error, "SDL_Init failed: %s", SDL_GetError());
      return false;
    }

    m_window = SDL_CreateWindow(
      cfg.title,
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      cfg.width, cfg.height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN
    );

    m_w = cfg.width;
    m_h = cfg.height;

    if (!m_window)
    {
      sc::log(sc::LogLevel::Error, "SDL_CreateWindow failed: %s", SDL_GetError());
      return false;
    }

    m_freq = now_freq();
    m_lastCounter = now_counter();

    sc::log(sc::LogLevel::Info, "Window created: %dx%d", cfg.width, cfg.height);
    return true;
  }

  void App::shutdown()
  {
    if (m_window)
    {
      SDL_DestroyWindow(m_window);
      m_window = nullptr;
    }
    SDL_Quit();
    sc::log(sc::LogLevel::Info, "Shutdown complete.");
  }

  bool App::pump()
  {
    SDL_Event e;
    
    while (SDL_PollEvent(&e))
    {
      if (e.type == SDL_QUIT) m_running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) m_running = false;

      if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
      {
        m_w = e.window.data1;
        m_h = e.window.data2;
        m_resized = true;
      }

      if (m_eventCb)
        m_eventCb(e, m_eventUser);
    }

    // dt measurement (for future frame pacing)
    const uint64_t c = now_counter();
    const double dt = static_cast<double>(c - m_lastCounter) / m_freq;
    m_lastCounter = c;

    // Temporary: print every ~2 seconds if you want (disabled to avoid spam)
    (void)dt;

    return m_running;
  }
}
