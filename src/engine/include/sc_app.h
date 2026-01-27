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
    SDL_Window* window() const { return m_window; }

    bool wasResized() const { return m_resized; }
    void clearResized() { m_resized = false; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    
  private:
    SDL_Window* m_window = nullptr;
    bool m_running = true;
    bool m_resized = false;
    int m_w = 0, m_h = 0;
    uint64_t m_lastCounter = 0;
    double m_freq = 0.0;
  };
}
