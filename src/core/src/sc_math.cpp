#include "sc_math.h"
#include <cassert>
#include <cmath>

#if defined(__AVX__) || defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <immintrin.h>
#endif

namespace sc
{
  Mat4 mat4_mul(const Mat4& a, const Mat4& b) noexcept
  {
#if defined(__AVX__)
    Mat4 r{};
    const float* bm = b.m;

    const __m128 a0 = _mm_loadu_ps(a.m + 0);
    const __m128 a1 = _mm_loadu_ps(a.m + 4);
    const __m128 a2 = _mm_loadu_ps(a.m + 8);
    const __m128 a3 = _mm_loadu_ps(a.m + 12);

    const __m256 a0_2 = _mm256_set_m128(a0, a0);
    const __m256 a1_2 = _mm256_set_m128(a1, a1);
    const __m256 a2_2 = _mm256_set_m128(a2, a2);
    const __m256 a3_2 = _mm256_set_m128(a3, a3);

    auto mul_pair = [&](int col0, int col1)
    {
      const int b0 = col0 * 4;
      const int b1 = col1 * 4;

      __m256 res = _mm256_mul_ps(a0_2, _mm256_set_ps(
        bm[b1 + 0], bm[b1 + 0], bm[b1 + 0], bm[b1 + 0],
        bm[b0 + 0], bm[b0 + 0], bm[b0 + 0], bm[b0 + 0]));
      res = _mm256_add_ps(res, _mm256_mul_ps(a1_2, _mm256_set_ps(
        bm[b1 + 1], bm[b1 + 1], bm[b1 + 1], bm[b1 + 1],
        bm[b0 + 1], bm[b0 + 1], bm[b0 + 1], bm[b0 + 1])));
      res = _mm256_add_ps(res, _mm256_mul_ps(a2_2, _mm256_set_ps(
        bm[b1 + 2], bm[b1 + 2], bm[b1 + 2], bm[b1 + 2],
        bm[b0 + 2], bm[b0 + 2], bm[b0 + 2], bm[b0 + 2])));
      res = _mm256_add_ps(res, _mm256_mul_ps(a3_2, _mm256_set_ps(
        bm[b1 + 3], bm[b1 + 3], bm[b1 + 3], bm[b1 + 3],
        bm[b0 + 3], bm[b0 + 3], bm[b0 + 3], bm[b0 + 3])));

      _mm_storeu_ps(r.m + b0, _mm256_castps256_ps128(res));
      _mm_storeu_ps(r.m + b1, _mm256_extractf128_ps(res, 1));
    };

    mul_pair(0, 1);
    mul_pair(2, 3);
    return r;
#elif defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    Mat4 r{};
    const __m128 a0 = _mm_loadu_ps(a.m + 0);
    const __m128 a1 = _mm_loadu_ps(a.m + 4);
    const __m128 a2 = _mm_loadu_ps(a.m + 8);
    const __m128 a3 = _mm_loadu_ps(a.m + 12);

    for (int col = 0; col < 4; ++col)
    {
      const float* bc = b.m + col * 4;
      __m128 res = _mm_mul_ps(a0, _mm_set1_ps(bc[0]));
      res = _mm_add_ps(res, _mm_mul_ps(a1, _mm_set1_ps(bc[1])));
      res = _mm_add_ps(res, _mm_mul_ps(a2, _mm_set1_ps(bc[2])));
      res = _mm_add_ps(res, _mm_mul_ps(a3, _mm_set1_ps(bc[3])));
      _mm_storeu_ps(r.m + col * 4, res);
    }
    return r;
#else
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
#endif
  }

  Mat4 mat4_transpose(const Mat4& a) noexcept
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

  Mat4 mat4_rotation_xyz(float rx, float ry, float rz) noexcept
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

  Mat4 mat4_trs(const float pos[3], const float rot[3], const float scale[3]) noexcept
  {
    assert(pos != nullptr);
    assert(rot != nullptr);
    assert(scale != nullptr);
    if (!pos || !rot || !scale)
      return Mat4::identity();

    const Mat4 t = mat4_translation(pos[0], pos[1], pos[2]);
    const Mat4 r = mat4_rotation_xyz(rot[0], rot[1], rot[2]);
    const Mat4 s = mat4_scale(scale[0], scale[1], scale[2]);
    return mat4_mul(t, mat4_mul(r, s));
  }

  Mat4 mat4_inverse(const Mat4& a) noexcept
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

    const float det = m[0] * o[0] + m[1] * o[4] + m[2] * o[8] + m[3] * o[12];
    if (std::fabs(det) <= EPSILON)
      return Mat4::identity();

    const float inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i)
      o[i] *= inv_det;

    return inv;
  }

  Mat4 mat4_perspective_rh_zo(float fovYRadians, float aspect, float zNear, float zFar, bool flipY) noexcept
  {
    assert(fovYRadians > 0.0f);
    assert(aspect > 0.0f);
    assert(zNear > 0.0f);
    assert(zFar > zNear);
    if (fovYRadians <= EPSILON || aspect <= EPSILON || zNear <= EPSILON || zFar <= zNear + EPSILON)
      return Mat4::identity();

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
