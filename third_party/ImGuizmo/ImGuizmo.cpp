#include "ImGuizmo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ImGuizmo
{
  namespace
  {
    struct Vec2
    {
      float x = 0.0f;
      float y = 0.0f;
    };

    struct Vec3
    {
      float x = 0.0f;
      float y = 0.0f;
      float z = 0.0f;
    };

    struct Context
    {
      ImDrawList* drawList = nullptr;
      ImVec2 rectMin = ImVec2(0, 0);
      ImVec2 rectMax = ImVec2(0, 0);
      bool orthographic = false;
      bool isOver = false;
      bool isUsing = false;
      bool active = false;
      OPERATION operation = TRANSLATE;
      MODE mode = LOCAL;
      int activeAxis = -1;
      float startMatrix[16]{};
      float startScale[3]{ 1.0f, 1.0f, 1.0f };
      float startAxisT = 0.0f;
      Vec3 startDir{};
    };

    static Context gCtx{};

    static Vec3 v3(float x, float y, float z) { return Vec3{ x, y, z }; }
    static Vec3 add(const Vec3& a, const Vec3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
    static Vec3 sub(const Vec3& a, const Vec3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
    static Vec3 mul(const Vec3& a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
    static float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static Vec3 cross(const Vec3& a, const Vec3& b)
    {
      return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
    }
    static float len(const Vec3& v) { return std::sqrt(dot(v, v)); }
    static Vec3 normalize(const Vec3& v)
    {
      const float l = len(v);
      if (l <= 1e-6f)
        return v3(0, 0, 0);
      return mul(v, 1.0f / l);
    }

    static void mat4Mul(const float* a, const float* b, float* out)
    {
      for (int col = 0; col < 4; ++col)
      {
        for (int row = 0; row < 4; ++row)
        {
          float sum = 0.0f;
          for (int k = 0; k < 4; ++k)
            sum += a[k * 4 + row] * b[col * 4 + k];
          out[col * 4 + row] = sum;
        }
      }
    }

    static void mat4MulVec4(const float* m, const float* v, float* out)
    {
      for (int row = 0; row < 4; ++row)
      {
        out[row] =
          m[0 * 4 + row] * v[0] +
          m[1 * 4 + row] * v[1] +
          m[2 * 4 + row] * v[2] +
          m[3 * 4 + row] * v[3];
      }
    }

    static bool mat4Inverse(const float* m, float* out)
    {
      float inv[16]{};

      inv[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15]
             + m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];

      inv[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15]
             - m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];

      inv[8] = m[4]  * m[9] * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5] * m[15]
             + m[8]  * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

      inv[12] = -m[4]  * m[9] * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5] * m[14]
              - m[8]  * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

      inv[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2] * m[15]
             - m[9]  * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

      inv[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2] * m[15]
             + m[8]  * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

      inv[9] = -m[0]  * m[9] * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1] * m[15]
             - m[8]  * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

      inv[13] = m[0]  * m[9] * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1] * m[14]
              + m[8]  * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

      inv[2] = m[1]  * m[6] * m[15] - m[1]  * m[7] * m[14] - m[5]  * m[2] * m[15]
             + m[5]  * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

      inv[6] = -m[0]  * m[6] * m[15] + m[0]  * m[7] * m[14] + m[4]  * m[2] * m[15]
             - m[4]  * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

      inv[10] = m[0]  * m[5] * m[15] - m[0]  * m[7] * m[13] - m[4]  * m[1] * m[15]
              + m[4]  * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

      inv[14] = -m[0]  * m[5] * m[14] + m[0]  * m[6] * m[13] + m[4]  * m[1] * m[14]
              - m[4]  * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

      inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
             - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

      inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
             + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

      inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
              - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

      inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
              + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

      float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
      if (std::fabs(det) < 1e-8f)
        return false;

      det = 1.0f / det;
      for (int i = 0; i < 16; ++i)
        out[i] = inv[i] * det;
      return true;
    }

    static float clampf(float v, float a, float b)
    {
      return std::max(a, std::min(b, v));
    }

    static bool isFinite(float v)
    {
      return std::isfinite(v);
    }

    static bool isFiniteVec3(const Vec3& v)
    {
      return isFinite(v.x) && isFinite(v.y) && isFinite(v.z);
    }

    static bool isFiniteMatrix(const float* m)
    {
      if (!m)
        return false;
      for (int i = 0; i < 16; ++i)
      {
        if (!isFinite(m[i]))
          return false;
      }
      return true;
    }

    static bool rectContains(const ImVec2& p)
    {
      return p.x >= gCtx.rectMin.x && p.y >= gCtx.rectMin.y &&
             p.x <= gCtx.rectMax.x && p.y <= gCtx.rectMax.y;
    }

    static bool worldToScreen(const float* viewProj, const Vec3& w, ImVec2* out)
    {
      float v[4] = { w.x, w.y, w.z, 1.0f };
      float clip[4]{};
      mat4MulVec4(viewProj, v, clip);
      if (clip[3] <= 1e-6f)
        return false;

      float ndc_x = clip[0] / clip[3];
      float ndc_y = clip[1] / clip[3];

      const float w_rect = std::max(1.0f, gCtx.rectMax.x - gCtx.rectMin.x);
      const float h_rect = std::max(1.0f, gCtx.rectMax.y - gCtx.rectMin.y);

      out->x = gCtx.rectMin.x + (ndc_x * 0.5f + 0.5f) * w_rect;
      out->y = gCtx.rectMin.y + (1.0f - (ndc_y * 0.5f + 0.5f)) * h_rect;
      return true;
    }

    struct Ray
    {
      Vec3 origin;
      Vec3 dir;
    };

    static Ray computeRay(const float* view, const float* proj, const ImVec2& mouse)
    {
      Ray ray{};

      float viewProj[16]{};
      mat4Mul(proj, view, viewProj);

      float invViewProj[16]{};
      mat4Inverse(viewProj, invViewProj);

      const float w_rect = std::max(1.0f, gCtx.rectMax.x - gCtx.rectMin.x);
      const float h_rect = std::max(1.0f, gCtx.rectMax.y - gCtx.rectMin.y);
      const float ndc_x = ((mouse.x - gCtx.rectMin.x) / w_rect) * 2.0f - 1.0f;
      const float ndc_y = 1.0f - ((mouse.y - gCtx.rectMin.y) / h_rect) * 2.0f;

      float near_clip[4] = { ndc_x, ndc_y, 0.0f, 1.0f };
      float far_clip[4]  = { ndc_x, ndc_y, 1.0f, 1.0f };
      float near_world[4]{};
      float far_world[4]{};
      mat4MulVec4(invViewProj, near_clip, near_world);
      mat4MulVec4(invViewProj, far_clip, far_world);

      if (std::fabs(near_world[3]) > 1e-6f)
      {
        near_world[0] /= near_world[3];
        near_world[1] /= near_world[3];
        near_world[2] /= near_world[3];
      }
      if (std::fabs(far_world[3]) > 1e-6f)
      {
        far_world[0] /= far_world[3];
        far_world[1] /= far_world[3];
        far_world[2] /= far_world[3];
      }

      float invView[16]{};
      mat4Inverse(view, invView);
      ray.origin = v3(invView[12], invView[13], invView[14]);
      ray.dir = normalize(sub(v3(far_world[0], far_world[1], far_world[2]), ray.origin));
      return ray;
    }

    static float distancePointToSegment2D(const ImVec2& p, const ImVec2& a, const ImVec2& b, float* out_t)
    {
      Vec2 pa{ p.x - a.x, p.y - a.y };
      Vec2 ba{ b.x - a.x, b.y - a.y };
      const float h = clampf((pa.x * ba.x + pa.y * ba.y) /
                             std::max(1e-6f, ba.x * ba.x + ba.y * ba.y), 0.0f, 1.0f);
      if (out_t)
        *out_t = h;
      const float dx = pa.x - ba.x * h;
      const float dy = pa.y - ba.y * h;
      return std::sqrt(dx * dx + dy * dy);
    }

    static bool closestPointRayAxis(const Ray& ray,
                                    const Vec3& axis,
                                    const Vec3& origin,
                                    float* out_axis_t,
                                    float denom_epsilon)
    {
      Vec3 v = axis;
      Vec3 w0 = sub(ray.origin, origin);
      float a = dot(v, v);
      float b = dot(v, ray.dir);
      float c = dot(ray.dir, ray.dir);
      float d = dot(v, w0);
      float e = dot(ray.dir, w0);
      float denom = a * c - b * b;
      if (std::fabs(denom) < denom_epsilon)
        return false;
      float s = (b * e - c * d) / denom;
      if (out_axis_t)
        *out_axis_t = s;
      return true;
    }

    static bool axisTFromRay(const Ray& ray,
                             const Vec3& axis,
                             const Vec3& origin,
                             const Vec3& view_dir,
                             float* out_axis_t)
    {
      const Vec3 axis_n = normalize(axis);
      const Vec3 view_n = normalize(view_dir);
      const Vec3 perp = cross(view_n, axis_n);
      Vec3 plane_n = cross(axis_n, perp);
      const float plane_len = len(plane_n);
      if (plane_len > 1e-4f)
      {
        plane_n = mul(plane_n, 1.0f / plane_len);
        const float denom = dot(ray.dir, plane_n);
        if (std::fabs(denom) > 1e-4f)
        {
          const float t = dot(sub(origin, ray.origin), plane_n) / denom;
          const Vec3 hit = add(ray.origin, mul(ray.dir, t));
          if (!isFiniteVec3(hit))
            return false;
          if (out_axis_t)
            *out_axis_t = dot(sub(hit, origin), axis_n);
          return true;
        }
      }

      float axis_t = 0.0f;
      if (closestPointRayAxis(ray, axis_n, origin, &axis_t, 1e-3f))
      {
        if (out_axis_t)
          *out_axis_t = axis_t;
        return true;
      }
      return false;
    }

    static void buildAxisAngleMatrix(const Vec3& axis_in, float angle, float* out)
    {
      Vec3 axis = normalize(axis_in);
      const float x = axis.x;
      const float y = axis.y;
      const float z = axis.z;
      const float c = std::cos(angle);
      const float s = std::sin(angle);
      const float t = 1.0f - c;

      // Row-major rotation values.
      const float r00 = t * x * x + c;
      const float r01 = t * x * y - s * z;
      const float r02 = t * x * z + s * y;
      const float r10 = t * x * y + s * z;
      const float r11 = t * y * y + c;
      const float r12 = t * y * z - s * x;
      const float r20 = t * x * z - s * y;
      const float r21 = t * y * z + s * x;
      const float r22 = t * z * z + c;

      std::memset(out, 0, sizeof(float) * 16);
      out[0] = r00; out[4] = r01; out[8]  = r02;
      out[1] = r10; out[5] = r11; out[9]  = r12;
      out[2] = r20; out[6] = r21; out[10] = r22;
      out[15] = 1.0f;
    }

    static void drawAxisLine(const ImVec2& a, const ImVec2& b, ImU32 color, float thickness)
    {
      if (gCtx.drawList)
        gCtx.drawList->AddLine(a, b, color, thickness);
    }

    static void drawCircle(const Vec3& origin,
                           const Vec3& axis,
                           float radius,
                           ImU32 color,
                           float thickness,
                           const float* viewProj)
    {
      if (!gCtx.drawList)
        return;
      Vec3 n = normalize(axis);
      Vec3 ref = (std::fabs(n.y) < 0.9f) ? v3(0, 1, 0) : v3(1, 0, 0);
      Vec3 u = normalize(cross(n, ref));
      Vec3 v = normalize(cross(n, u));
      const int segments = 64;

      ImVec2 prev{};
      bool prev_ok = false;
      for (int i = 0; i <= segments; ++i)
      {
        const float t = (static_cast<float>(i) / segments) * 6.283185307f;
        Vec3 p = add(origin, add(mul(u, std::cos(t) * radius), mul(v, std::sin(t) * radius)));
        ImVec2 s{};
        if (worldToScreen(viewProj, p, &s))
        {
          if (prev_ok)
            gCtx.drawList->AddLine(prev, s, color, thickness);
          prev = s;
          prev_ok = true;
        }
        else
        {
          prev_ok = false;
        }
      }
    }
  }

  void SetOrthographic(bool is_ortho)
  {
    gCtx.orthographic = is_ortho;
  }

  void SetDrawlist(ImDrawList* drawlist)
  {
    gCtx.drawList = drawlist ? drawlist : ImGui::GetWindowDrawList();
  }

  void SetRect(float x, float y, float width, float height)
  {
    gCtx.rectMin = ImVec2(x, y);
    gCtx.rectMax = ImVec2(x + width, y + height);
  }

  bool Manipulate(const float* view,
                  const float* projection,
                  OPERATION operation,
                  MODE mode,
                  float* matrix,
                  float* /*deltaMatrix*/,
                  const float* snap,
                  const float* /*localBounds*/,
                  const float* /*boundsSnap*/)
  {
    if (!view || !projection || !matrix)
      return false;
    if (!isFiniteMatrix(view) || !isFiniteMatrix(projection) || !isFiniteMatrix(matrix))
      return false;

    gCtx.operation = operation;
    gCtx.mode = mode;
    gCtx.isOver = false;
    gCtx.isUsing = false;

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool mouse_in_rect = rectContains(mouse);

    float viewProj[16]{};
    mat4Mul(projection, view, viewProj);

    Vec3 origin = v3(matrix[12], matrix[13], matrix[14]);

    Vec3 axis_world[3] = { v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1) };
    if (mode == LOCAL)
    {
      Vec3 col0 = v3(matrix[0], matrix[1], matrix[2]);
      Vec3 col1 = v3(matrix[4], matrix[5], matrix[6]);
      Vec3 col2 = v3(matrix[8], matrix[9], matrix[10]);
      axis_world[0] = normalize(col0);
      axis_world[1] = normalize(col1);
      axis_world[2] = normalize(col2);
    }

    float invView[16]{};
    mat4Inverse(view, invView);
    Vec3 cam_pos = v3(invView[12], invView[13], invView[14]);
    Vec3 view_dir = normalize(v3(invView[8], invView[9], invView[10]));
    const float dist = std::max(0.25f, len(sub(cam_pos, origin)));
    const float axis_len = std::max(0.25f, dist * 0.15f);
    const float max_delta = std::max(1.0f, axis_len * 100.0f);

    const ImU32 colors[3] = {
      IM_COL32(230, 80, 80, 255),
      IM_COL32(80, 230, 80, 255),
      IM_COL32(80, 120, 230, 255)
    };

    const float line_thickness = 2.0f;
    const float hover_thickness = 3.5f;

    // Draw gizmo.
    if (operation == TRANSLATE || operation == SCALE)
    {
      for (int i = 0; i < 3; ++i)
      {
        Vec3 end = add(origin, mul(axis_world[i], axis_len));
        ImVec2 s0{}, s1{};
        if (worldToScreen(viewProj, origin, &s0) && worldToScreen(viewProj, end, &s1))
          drawAxisLine(s0, s1, colors[i], line_thickness);
      }
    }
    else if (operation == ROTATE)
    {
      const float radius = axis_len * 0.9f;
      for (int i = 0; i < 3; ++i)
        drawCircle(origin, axis_world[i], radius, colors[i], line_thickness, viewProj);
    }

    // Determine hovered axis when not active.
    int hovered_axis = -1;
    if (!gCtx.active && mouse_in_rect)
    {
      if (operation == TRANSLATE || operation == SCALE)
      {
        float best = 1e30f;
        const float threshold = 12.0f;
        ImVec2 s0{};
        ImVec2 s1{};
        for (int i = 0; i < 3; ++i)
        {
          Vec3 end = add(origin, mul(axis_world[i], axis_len));
          if (worldToScreen(viewProj, origin, &s0) && worldToScreen(viewProj, end, &s1))
          {
            const float dist_px = distancePointToSegment2D(mouse, s0, s1, nullptr);
            if (dist_px < threshold && dist_px < best)
            {
              best = dist_px;
              hovered_axis = i;
            }
          }
        }
      }
      else if (operation == ROTATE)
      {
        Ray ray = computeRay(view, projection, mouse);
        const float radius = axis_len * 0.9f;
        float best = 1e30f;
        const float threshold = radius * 0.12f;
        for (int i = 0; i < 3; ++i)
        {
          Vec3 axis = axis_world[i];
          const float denom = dot(ray.dir, axis);
          if (std::fabs(denom) < 1e-6f)
            continue;
          const float t = dot(sub(origin, ray.origin), axis) / denom;
          if (t < 0.0f)
            continue;
          Vec3 hit = add(ray.origin, mul(ray.dir, t));
          const float d = len(sub(hit, origin));
          const float diff = std::fabs(d - radius);
          if (diff < threshold && diff < best)
          {
            best = diff;
            hovered_axis = i;
          }
        }
      }
    }

    gCtx.isOver = (hovered_axis >= 0);

    if (!gCtx.active && hovered_axis >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
      gCtx.active = true;
      gCtx.activeAxis = hovered_axis;
      std::memcpy(gCtx.startMatrix, matrix, sizeof(float) * 16);

      Vec3 col0 = v3(matrix[0], matrix[1], matrix[2]);
      Vec3 col1 = v3(matrix[4], matrix[5], matrix[6]);
      Vec3 col2 = v3(matrix[8], matrix[9], matrix[10]);
      gCtx.startScale[0] = len(col0);
      gCtx.startScale[1] = len(col1);
      gCtx.startScale[2] = len(col2);

      if (operation == TRANSLATE || operation == SCALE)
      {
        Ray ray = computeRay(view, projection, mouse);
        float axis_t = 0.0f;
        if (axisTFromRay(ray, axis_world[hovered_axis], origin, view_dir, &axis_t))
          gCtx.startAxisT = axis_t;
      }
      else if (operation == ROTATE)
      {
        Ray ray = computeRay(view, projection, mouse);
        Vec3 axis = axis_world[hovered_axis];
        float denom = dot(ray.dir, axis);
        if (std::fabs(denom) > 1e-6f)
        {
          float t = dot(sub(origin, ray.origin), axis) / denom;
          Vec3 hit = add(ray.origin, mul(ray.dir, t));
          gCtx.startDir = normalize(sub(hit, origin));
        }
      }
    }

    bool modified = false;
    if (gCtx.active)
    {
      gCtx.isUsing = ImGui::IsMouseDown(ImGuiMouseButton_Left);

      if (gCtx.isUsing)
      {
        if (operation == TRANSLATE)
        {
          Ray ray = computeRay(view, projection, mouse);
          float axis_t = 0.0f;
          if (axisTFromRay(ray, axis_world[gCtx.activeAxis], origin, view_dir, &axis_t))
          {
            float delta = axis_t - gCtx.startAxisT;
            if (!isFinite(delta) || std::fabs(delta) > max_delta)
              return false;
            const float snap_step = snap ? snap[gCtx.activeAxis] : 0.0f;
            if (snap_step > 0.0f)
              delta = std::round(delta / snap_step) * snap_step;
            Vec3 new_pos = add(origin, mul(axis_world[gCtx.activeAxis], delta));
            if (!isFiniteVec3(new_pos))
              return false;
            std::memcpy(matrix, gCtx.startMatrix, sizeof(float) * 16);
            matrix[12] = new_pos.x;
            matrix[13] = new_pos.y;
            matrix[14] = new_pos.z;
            modified = true;
          }
        }
        else if (operation == ROTATE)
        {
          Ray ray = computeRay(view, projection, mouse);
          Vec3 axis = axis_world[gCtx.activeAxis];
          float denom = dot(ray.dir, axis);
          if (std::fabs(denom) > 1e-6f)
          {
            float t = dot(sub(origin, ray.origin), axis) / denom;
            Vec3 hit = add(ray.origin, mul(ray.dir, t));
            Vec3 dir = normalize(sub(hit, origin));
            float c = dot(gCtx.startDir, dir);
            Vec3 cr = cross(gCtx.startDir, dir);
            float s = dot(axis, cr);
            float angle = std::atan2(s, c);
            if (!isFinite(angle))
              return false;
            if (snap && snap[0] > 0.0f)
            {
              const float snap_rad = snap[0] * 0.0174532925f;
              angle = std::round(angle / snap_rad) * snap_rad;
            }

            float r[16]{};
            if (mode == LOCAL)
            {
              Vec3 axis_local = (gCtx.activeAxis == 0) ? v3(1, 0, 0) :
                                (gCtx.activeAxis == 1) ? v3(0, 1, 0) : v3(0, 0, 1);
              buildAxisAngleMatrix(axis_local, angle, r);
              mat4Mul(gCtx.startMatrix, r, matrix);
            }
            else
            {
              buildAxisAngleMatrix(axis, angle, r);
              mat4Mul(r, gCtx.startMatrix, matrix);
            }
            if (!isFiniteMatrix(matrix))
              return false;
            modified = true;
          }
        }
        else if (operation == SCALE)
        {
          Ray ray = computeRay(view, projection, mouse);
          float axis_t = 0.0f;
          if (axisTFromRay(ray, axis_world[gCtx.activeAxis], origin, view_dir, &axis_t))
          {
            float delta = axis_t - gCtx.startAxisT;
            if (!isFinite(delta) || std::fabs(delta) > max_delta)
              return false;
            float new_scale = gCtx.startScale[gCtx.activeAxis] + delta;
            const float snap_step = snap ? snap[gCtx.activeAxis] : 0.0f;
            if (snap_step > 0.0f)
              new_scale = std::round(new_scale / snap_step) * snap_step;
            new_scale = std::max(0.001f, new_scale);

            const float base_scale = std::max(0.001f, gCtx.startScale[gCtx.activeAxis]);
            const float factor = new_scale / base_scale;
            std::memcpy(matrix, gCtx.startMatrix, sizeof(float) * 16);
            const int col = gCtx.activeAxis * 4;
            matrix[col + 0] *= factor;
            matrix[col + 1] *= factor;
            matrix[col + 2] *= factor;
            if (!isFiniteMatrix(matrix))
              return false;
            modified = true;
          }
        }
      }

      if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
      {
        gCtx.active = false;
        gCtx.activeAxis = -1;
        gCtx.isUsing = false;
      }
    }

    return modified;
  }

  bool IsOver()
  {
    return gCtx.isOver;
  }

  bool IsUsing()
  {
    return gCtx.isUsing;
  }

  void DecomposeMatrixToComponents(const float* matrix,
                                   float* translation,
                                   float* rotation,
                                   float* scale)
  {
    if (!matrix)
      return;

    if (translation)
    {
      translation[0] = matrix[12];
      translation[1] = matrix[13];
      translation[2] = matrix[14];
    }

    Vec3 col0 = v3(matrix[0], matrix[1], matrix[2]);
    Vec3 col1 = v3(matrix[4], matrix[5], matrix[6]);
    Vec3 col2 = v3(matrix[8], matrix[9], matrix[10]);

    float sx = len(col0);
    float sy = len(col1);
    float sz = len(col2);
    if (scale)
    {
      scale[0] = sx;
      scale[1] = sy;
      scale[2] = sz;
    }

    Vec3 n0 = (sx > 1e-6f) ? mul(col0, 1.0f / sx) : v3(1, 0, 0);
    Vec3 n1 = (sy > 1e-6f) ? mul(col1, 1.0f / sy) : v3(0, 1, 0);
    Vec3 n2 = (sz > 1e-6f) ? mul(col2, 1.0f / sz) : v3(0, 0, 1);

    if (dot(cross(n0, n1), n2) < 0.0f)
    {
      sx = -sx;
      n0 = mul(n0, -1.0f);
      if (scale)
        scale[0] = sx;
    }

    // Rotation matrix elements (column-major).
    const float r00 = n0.x;
    const float r01 = n1.x;
    const float r02 = n2.x;
    const float r10 = n0.y;
    const float r11 = n1.y;
    const float r12 = n2.y;
    const float r20 = n0.z;
    const float r21 = n1.z;
    const float r22 = n2.z;

    float ry = std::asin(clampf(r20, -1.0f, 1.0f));
    float cy = std::cos(ry);
    float rx = 0.0f;
    float rz = 0.0f;
    if (std::fabs(cy) > 1e-5f)
    {
      rx = std::atan2(-r21, r22);
      rz = std::atan2(-r10, r00);
    }
    else
    {
      rx = 0.0f;
      rz = std::atan2(r01, r11);
    }

    if (rotation)
    {
      const float rad_to_deg = 57.2957795f;
      rotation[0] = rx * rad_to_deg;
      rotation[1] = ry * rad_to_deg;
      rotation[2] = rz * rad_to_deg;
    }
  }

  void RecomposeMatrixFromComponents(const float* translation,
                                     const float* rotation,
                                     const float* scale,
                                     float* matrix)
  {
    if (!matrix)
      return;

    const float tx = translation ? translation[0] : 0.0f;
    const float ty = translation ? translation[1] : 0.0f;
    const float tz = translation ? translation[2] : 0.0f;
    const float sx = scale ? scale[0] : 1.0f;
    const float sy = scale ? scale[1] : 1.0f;
    const float sz = scale ? scale[2] : 1.0f;
    const float rx = rotation ? rotation[0] * 0.0174532925f : 0.0f;
    const float ry = rotation ? rotation[1] * 0.0174532925f : 0.0f;
    const float rz = rotation ? rotation[2] * 0.0174532925f : 0.0f;

    const float cx = std::cos(rx);
    const float sxr = std::sin(rx);
    const float cy = std::cos(ry);
    const float syr = std::sin(ry);
    const float cz = std::cos(rz);
    const float szr = std::sin(rz);

    float r[16]{};
    r[0] = cz * cy;
    r[4] = cz * syr * sxr + szr * cx;
    r[8] = -cz * syr * cx + szr * sxr;

    r[1] = -szr * cy;
    r[5] = -szr * syr * sxr + cz * cx;
    r[9] = szr * syr * cx + cz * sxr;

    r[2] = syr;
    r[6] = -cy * sxr;
    r[10] = cy * cx;
    r[15] = 1.0f;

    float s[16]{};
    s[0] = sx; s[5] = sy; s[10] = sz; s[15] = 1.0f;

    float rs[16]{};
    mat4Mul(r, s, rs);
    std::memcpy(matrix, rs, sizeof(float) * 16);
    matrix[12] = tx;
    matrix[13] = ty;
    matrix[14] = tz;
    matrix[15] = 1.0f;
  }
}
