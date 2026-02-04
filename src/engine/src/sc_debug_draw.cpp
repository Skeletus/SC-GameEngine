#include "sc_debug_draw.h"

#include <algorithm>
#include <cmath>

namespace sc
{
  static inline DebugVertex make_vertex(const float p[3], const float c[3])
  {
    DebugVertex v{};
    v.pos[0] = p[0]; v.pos[1] = p[1]; v.pos[2] = p[2];
    v.color[0] = c[0]; v.color[1] = c[1]; v.color[2] = c[2];
    return v;
  }

  void DebugDraw::addLine(const float p0[3], const float p1[3], const float color[3])
  {
    m_vertices.push_back(make_vertex(p0, color));
    m_vertices.push_back(make_vertex(p1, color));
  }

  void DebugDraw::addGrid(float size, float step)
  {
    const float gridSize = (size > 0.0f) ? size : 1.0f;
    const float gridStep = (step > 0.001f) ? step : 1.0f;

    const float gridColor[3] = { 0.35f, 0.35f, 0.38f };
    const float xColor[3] = { 0.90f, 0.20f, 0.20f };
    const float yColor[3] = { 0.20f, 0.90f, 0.20f };
    const float zColor[3] = { 0.20f, 0.45f, 0.95f };

    const float half = gridSize;
    const int lineCount = (int)std::floor(half / gridStep);

    for (int i = -lineCount; i <= lineCount; ++i)
    {
      const float v = (float)i * gridStep;

      const float p0[3] = { -half, 0.0f, v };
      const float p1[3] = {  half, 0.0f, v };
      addLine(p0, p1, gridColor);

      const float p2[3] = { v, 0.0f, -half };
      const float p3[3] = { v, 0.0f,  half };
      addLine(p2, p3, gridColor);
    }

    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float xEnd[3] = { half, 0.0f, 0.0f };
    const float yEnd[3] = { 0.0f, half, 0.0f };
    const float zEnd[3] = { 0.0f, 0.0f, half };

    addLine(origin, xEnd, xColor);
    addLine(origin, yEnd, yColor);
    addLine(origin, zEnd, zColor);
  }

  void DebugDrawSystem(World& world, float dt, void* user)
  {
    (void)world;
    (void)dt;
    DebugDraw* draw = static_cast<DebugDraw*>(user);
    if (!draw)
      return;

    draw->clear();
    const DebugDrawSettings& s = draw->settings();
    if (s.showGrid)
      draw->addGrid(s.gridSize, s.gridStep);
  }
}
