#include "sc_math.h"
#include <cmath>

namespace sc
{
  Mat4 Mat4::identity()
  {
    Mat4 r{};
    r.m[0] = 1.0f;
    r.m[5] = 1.0f;
    r.m[10] = 1.0f;
    r.m[15] = 1.0f;
    return r;
  }

  Mat4 mat4_identity()
  {
    return Mat4::identity();
  }

  Mat4 mat4_mul(const Mat4& a, const Mat4& b)
  {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
    {
      for (int row = 0; row < 4; ++row)
      {
        float sum = 0.0f;
        for (int k = 0; k < 4; ++k)
        {
          sum += a.m[k * 4 + row] * b.m[col * 4 + k];
        }
        r.m[col * 4 + row] = sum;
      }
    }
    return r;
  }

  Mat4 mat4_transpose(const Mat4& a)
  {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
    {
      for (int row = 0; row < 4; ++row)
      {
        r.m[col * 4 + row] = a.m[row * 4 + col];
      }
    }
    return r;
  }

  Mat4 mat4_translation(float x, float y, float z)
  {
    Mat4 r = Mat4::identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
  }

  Mat4 mat4_scale(float x, float y, float z)
  {
    Mat4 r{};
    r.m[0] = x;
    r.m[5] = y;
    r.m[10] = z;
    r.m[15] = 1.0f;
    return r;
  }

  Mat4 mat4_rotation_xyz(float rx, float ry, float rz)
  {
    const float cx = std::cos(rx);
    const float sx = std::sin(rx);
    const float cy = std::cos(ry);
    const float sy = std::sin(ry);
    const float cz = std::cos(rz);
    const float sz = std::sin(rz);

    Mat4 rxm = Mat4::identity();
    rxm.m[5] = cx;
    rxm.m[6] = sx;
    rxm.m[9] = -sx;
    rxm.m[10] = cx;

    Mat4 rym = Mat4::identity();
    rym.m[0] = cy;
    rym.m[2] = -sy;
    rym.m[8] = sy;
    rym.m[10] = cy;

    Mat4 rzm = Mat4::identity();
    rzm.m[0] = cz;
    rzm.m[1] = sz;
    rzm.m[4] = -sz;
    rzm.m[5] = cz;

    return mat4_mul(mat4_mul(rzm, rym), rxm);
  }

  Mat4 mat4_trs(const float pos[3], const float rot[3], const float scale[3])
  {
    const Mat4 t = mat4_translation(pos[0], pos[1], pos[2]);
    const Mat4 r = mat4_rotation_xyz(rot[0], rot[1], rot[2]);
    const Mat4 s = mat4_scale(scale[0], scale[1], scale[2]);
    return mat4_mul(t, mat4_mul(r, s));
  }

  Mat4 mat4_inverse(const Mat4& a)
  {
    const float* m = a.m;
    Mat4 inv{};
    float* o = inv.m;

    o[0] = m[5]  * m[10] * m[15] - m[5]  * m[11] * m[14] - m[9]  * m[6]  * m[15]
         + m[9]  * m[7]  * m[14] + m[13] * m[6]  * m[11] - m[13] * m[7]  * m[10];

    o[4] = -m[4]  * m[10] * m[15] + m[4]  * m[11] * m[14] + m[8]  * m[6]  * m[15]
         - m[8]  * m[7]  * m[14] - m[12] * m[6]  * m[11] + m[12] * m[7]  * m[10];

    o[8] = m[4]  * m[9] * m[15] - m[4]  * m[11] * m[13] - m[8]  * m[5] * m[15]
         + m[8]  * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];

    o[12] = -m[4]  * m[9] * m[14] + m[4]  * m[10] * m[13] + m[8]  * m[5] * m[14]
          - m[8]  * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    o[1] = -m[1]  * m[10] * m[15] + m[1]  * m[11] * m[14] + m[9]  * m[2] * m[15]
         - m[9]  * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];

    o[5] = m[0]  * m[10] * m[15] - m[0]  * m[11] * m[14] - m[8]  * m[2] * m[15]
         + m[8]  * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];

    o[9] = -m[0]  * m[9] * m[15] + m[0]  * m[11] * m[13] + m[8]  * m[1] * m[15]
         - m[8]  * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];

    o[13] = m[0]  * m[9] * m[14] - m[0]  * m[10] * m[13] - m[8]  * m[1] * m[14]
          + m[8]  * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    o[2] = m[1]  * m[6] * m[15] - m[1]  * m[7] * m[14] - m[5]  * m[2] * m[15]
         + m[5]  * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];

    o[6] = -m[0]  * m[6] * m[15] + m[0]  * m[7] * m[14] + m[4]  * m[2] * m[15]
         - m[4]  * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];

    o[10] = m[0]  * m[5] * m[15] - m[0]  * m[7] * m[13] - m[4]  * m[1] * m[15]
          + m[4]  * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];

    o[14] = -m[0]  * m[5] * m[14] + m[0]  * m[6] * m[13] + m[4]  * m[1] * m[14]
          - m[4]  * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    o[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11]
         - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];

    o[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11]
         + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];

    o[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11]
          - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];

    o[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10]
          + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * o[0] + m[1] * o[4] + m[2] * o[8] + m[3] * o[12];
    if (det == 0.0f)
      return Mat4::identity();

    det = 1.0f / det;
    for (int i = 0; i < 16; ++i)
      o[i] *= det;

    return inv;
  }

  Mat4 mat4_perspective_rh_zo(float fovYRadians, float aspect, float zNear, float zFar, bool flipY)
  {
    Mat4 r{};
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);

    r.m[0] = f / aspect;
    r.m[5] = flipY ? -f : f;

    // RH, depth 0..1
    r.m[10] = zFar / (zNear - zFar);
    r.m[14] = (zFar * zNear) / (zNear - zFar);

    // esta es la clave:
    r.m[11] = -1.0f;

    return r;
  }

}
