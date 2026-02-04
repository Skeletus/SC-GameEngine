#pragma once
#include <cstdint>

namespace sc
{
  struct Mat4
  {
    alignas(16) float m[16]{};

    static Mat4 identity();
  };

  Mat4 mat4_identity();
  Mat4 mat4_mul(const Mat4& a, const Mat4& b);
  Mat4 mat4_transpose(const Mat4& a);
  Mat4 mat4_translation(float x, float y, float z);
  Mat4 mat4_scale(float x, float y, float z);
  Mat4 mat4_rotation_xyz(float rx, float ry, float rz);
  Mat4 mat4_trs(const float pos[3], const float rot[3], const float scale[3]);
  Mat4 mat4_inverse(const Mat4& a);

  // Right-handed, depth 0..1 (Vulkan). Set flipY=true to keep +Y up in world.
  Mat4 mat4_perspective_rh_zo(float fovYRadians, float aspect, float zNear, float zFar, bool flipY);
}
