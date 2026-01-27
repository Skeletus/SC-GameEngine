#pragma once
#include <cstdint>

struct SDL_Window;

namespace sc
{
  struct AppConfig
  {
    const char* title = "SandboxCityEngine";
    int width = 1280;
    int height = 720;
    bool vsync = true;
  };

  class App
  {
  public:
    bool init(const AppConfig& cfg);
    void shutdown();

    // returns false when should quit
    bool pump();

  private:
    SDL_Window* m_window = nullptr;
    bool m_running = true;
    uint64_t m_lastCounter = 0;
    double m_freq = 0.0;
  };
}
