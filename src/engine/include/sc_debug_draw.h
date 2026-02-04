#pragma once
#include <vector>
#include <cstdint>

#include "sc_ecs.h"

namespace sc
{
  struct DebugVertex
  {
    float pos[3]{};
    float color[3]{};
  };

  struct DebugDrawSettings
  {
    bool showGrid = true;
    float gridSize = 10.0f;
    float gridStep = 1.0f;
  };

  class DebugDraw
  {
  public:
    void clear() { m_vertices.clear(); }
    void reserve(uint32_t vertexCount) { m_vertices.reserve(vertexCount); }

    void addLine(const float p0[3], const float p1[3], const float color[3]);
    void addGrid(float size, float step);

    const std::vector<DebugVertex>& vertices() const { return m_vertices; }
    DebugDrawSettings& settings() { return m_settings; }
    const DebugDrawSettings& settings() const { return m_settings; }

  private:
    std::vector<DebugVertex> m_vertices;
    DebugDrawSettings m_settings{};
  };

  void DebugDrawSystem(World& world, float dt, void* user);
}
