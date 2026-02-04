#include "sc_debug_draw.h"
#include "sc_world_partition.h"

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

  static void addAabb(DebugDraw& draw, const AABB& box, const float color[3])
  {
    const float x0 = box.min.x;
    const float y0 = box.min.y;
    const float z0 = box.min.z;
    const float x1 = box.max.x;
    const float y1 = box.max.y;
    const float z1 = box.max.z;

    const float p000[3] = { x0, y0, z0 };
    const float p001[3] = { x0, y0, z1 };
    const float p010[3] = { x0, y1, z0 };
    const float p011[3] = { x0, y1, z1 };
    const float p100[3] = { x1, y0, z0 };
    const float p101[3] = { x1, y0, z1 };
    const float p110[3] = { x1, y1, z0 };
    const float p111[3] = { x1, y1, z1 };

    draw.addLine(p000, p001, color);
    draw.addLine(p001, p101, color);
    draw.addLine(p101, p100, color);
    draw.addLine(p100, p000, color);

    draw.addLine(p010, p011, color);
    draw.addLine(p011, p111, color);
    draw.addLine(p111, p110, color);
    draw.addLine(p110, p010, color);

    draw.addLine(p000, p010, color);
    draw.addLine(p001, p011, color);
    draw.addLine(p101, p111, color);
    draw.addLine(p100, p110, color);
  }

  void DebugDrawSystem(World& world, float dt, void* user)
  {
    (void)dt;
    DebugDrawSystemState* ctx = static_cast<DebugDrawSystemState*>(user);
    if (!ctx || !ctx->draw)
      return;

    DebugDraw* draw = ctx->draw;
    draw->clear();
    const DebugDrawSettings& s = draw->settings();
    if (s.showGrid)
      draw->addGrid(s.gridSize, s.gridStep);

    if (ctx->streaming && ctx->streaming->showSectorBounds)
    {
      const SectorCoord cameraSector = ctx->streaming->stats.cameraSector;
      const float currentColor[3] = { 1.0f, 0.9f, 0.2f };
      const float neighborColor[3] = { 0.3f, 1.0f, 0.45f };
      const float loadedColor[3] = { 0.25f, 0.7f, 1.0f };

      for (const auto& pair : ctx->streaming->partition.sectors())
      {
        const Sector& sector = pair.second;
        if (sector.state != SectorLoadState::Loaded)
          continue;

        const int dx = sector.coord.x - cameraSector.x;
        const int dz = sector.coord.z - cameraSector.z;
        const int manhattan = std::abs(dx) + std::abs(dz);
        const float* color = loadedColor;
        if (dx == 0 && dz == 0)
          color = currentColor;
        else if (manhattan == 1)
          color = neighborColor;

        addAabb(*draw, ctx->streaming->partition.sectorBounds(sector.coord), color);
      }
    }

    if (ctx->streaming && ctx->streaming->showEntityBounds && ctx->culling)
    {
      uint32_t remaining = ctx->streaming->entityBoundsLimit;
      const float visibleColor[3] = { 0.2f, 0.9f, 0.3f };
      const float culledColor[3] = { 0.95f, 0.25f, 0.2f };

      auto drawEntityBounds = [&](const std::vector<Entity>& entities, const float color[3])
      {
        for (const Entity e : entities)
        {
          if (remaining == 0)
            break;

          Transform* t = world.get<Transform>(e);
          Bounds* b = world.get<Bounds>(e);
          if (!t || !b)
            continue;

          float center[3]{};
          float radius = 0.0f;
          computeWorldBoundsSphere(*t, *b, center, radius);

          AABB box{};
          box.min = { center[0] - radius, center[1] - radius, center[2] - radius };
          box.max = { center[0] + radius, center[1] + radius, center[2] + radius };
          addAabb(*draw, box, color);
          remaining--;
        }
      };

      drawEntityBounds(ctx->culling->visible, visibleColor);
      drawEntityBounds(ctx->culling->culled, culledColor);
    }
  }
}
