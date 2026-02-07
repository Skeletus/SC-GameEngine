#pragma once
#include <cstdint>

namespace sc
{
  inline constexpr float EPSILON = 1e-6f;

  struct alignas(16) Mat4
  {
    alignas(16) float m[16]{};

    [[nodiscard]] static constexpr Mat4 identity() noexcept
    {
      Mat4 r{};
      r.m[0] = 1.0f;
      r.m[5] = 1.0f;
      r.m[10] = 1.0f;
      r.m[15] = 1.0f;
      return r;
    }
  };

  static_assert(sizeof(Mat4) == sizeof(float) * 16, "Mat4 must be 16 floats.");
  static_assert(alignof(Mat4) >= 16, "Mat4 must be at least 16-byte aligned.");

  [[nodiscard]] inline constexpr Mat4 mat4_identity() noexcept
  {
    return Mat4::identity();
  }

  [[nodiscard]] Mat4 mat4_mul(const Mat4& a, const Mat4& b) noexcept;
  [[nodiscard]] Mat4 mat4_transpose(const Mat4& a) noexcept;

  [[nodiscard]] inline constexpr Mat4 mat4_translation(float x, float y, float z) noexcept
  {
    Mat4 r = Mat4::identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
  }

  [[nodiscard]] inline constexpr Mat4 mat4_scale(float x, float y, float z) noexcept
  {
    Mat4 r{};
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    r.m[15] = 1.0f;
    return r;
  }

  [[nodiscard]] Mat4 mat4_rotation_xyz(float rx, float ry, float rz) noexcept;
  [[nodiscard]] Mat4 mat4_trs(const float pos[3], const float rot[3], const float scale[3]) noexcept;
  [[nodiscard]] Mat4 mat4_inverse(const Mat4& a) noexcept;

  // Right-handed, depth 0..1 (Vulkan). Set flipY=true to keep +Y up in world.
  [[nodiscard]] Mat4 mat4_perspective_rh_zo(float fovYRadians, float aspect, float zNear, float zFar, bool flipY) noexcept;
}
