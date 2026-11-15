// SPDX-License-Identifier: Apache-2.0
// Device-side scalar helpers.  Every float operation is one IEEE binary32 operation (build with -ffp-contract=off, no fast-math).
// Math and orders of operations: spec/A_gemv.md, spec/B_layers.md.
#pragma once
#include <sycl/sycl.hpp>
#include <cstdint>
namespace yx {
constexpr float K512 = 0.044194173824159216f;
constexpr int H = 5120, SST = 16;
inline float u2f(uint32_t u) { return sycl::bit_cast<float>(u); }
inline uint32_t f2u(float f) { return sycl::bit_cast<uint32_t>(f); }
// The device's float divide and sqrt are not correctly rounded by default (about 1 ulp off in ~27% of divisions on Xe2).
// The engine MUST be built with  -Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt"  (tests/div_test checks it):
// with it both are IEEE binary32, at no measurable cost.  Do not route through double: the compiler folds it back.
inline float fdiv(float a, float b) { return a / b; }
inline float fsqrt(float a) { return sycl::sqrt(a); }
inline float bf2f(uint16_t u) { return u2f((uint32_t)u << 16); }
inline uint16_t f2bf(float f) { uint32_t u = f2u(f); u += 0x7fffu + ((u >> 16) & 1u); return (uint16_t)(u >> 16); }
inline float rbf(float f) { return bf2f(f2bf(f)); }
// float -> binary16 -> float, round to nearest even (written out: no dependence on the device's half conversion)
inline float rf16(float f) {
  const uint32_t u = f2u(f), sign = u & 0x80000000u, a = u & 0x7fffffffu;
  if (a >= 0x7f800000u) return f;                                   // inf / nan
  if (a >= 0x477ff000u) return u2f(sign | 0x7f800000u);             // >= 65520 -> inf
  if (a < 0x38800000u) {                                            // below 2^-14: subnormal halves, spacing 2^-24
    const float s = u2f(a) * 16777216.f;                            // exact
    const float r = sycl::rint(s);                                  // ties to even
    return u2f(sign | f2u(r * 5.9604644775390625e-08f));
  }
  uint32_t m = a + 0xfffu + ((a >> 13) & 1u);                       // keep 10 mantissa bits
  return u2f(sign | (m & 0xffffe000u));
}
inline void fwht512(float* x) {
  for (int h = 1; h < 512; h <<= 1)
    for (int p = 0; p < 512; p += 2 * h)
      for (int i = p; i < p + h; ++i) { const float a = x[i], b = x[i + h]; x[i] = a + b; x[i + h] = a - b; }
}
inline float xor_tree32(const float* a) {
  float b[32], c[32];
  for (int i = 0; i < 32; ++i) b[i] = a[i] + a[i ^ 16];
  for (int i = 0; i < 32; ++i) c[i] = b[i] + b[i ^ 8];
  for (int i = 0; i < 32; ++i) b[i] = c[i] + c[i ^ 4];
  for (int i = 0; i < 32; ++i) c[i] = b[i] + b[i ^ 2];
  return c[0] + c[1];
}
inline float tree128(const float* a) { return ((xor_tree32(a) + xor_tree32(a + 32)) + xor_tree32(a + 64)) + xor_tree32(a + 96); }
// ---- portable math (pm_math.cuh) ----
inline float pm_rsqrt(float x) { return fdiv(1.f, fsqrt(x)); }
inline float pm_expf(float x) {
  const uint32_t ux = f2u(x), ax = ux & 0x7fffffffu; const bool neg = ux >> 31;
  if (ax > 0x7f800000u) return x + x;
  if (!neg && ax >= 0x42b17218u) return u2f(0x7f800000u);
  if (ux > 0xc2cff1b5u) return 0.f;
  const float LN2HI = u2f(0x3f317200u), LN2LO = u2f(0x35bfbe8eu);
  float hi = 0.f, lo = 0.f, r = x; int k = 0;
  if (ax > 0x3eb17218u) {
    if (ax < 0x3f851592u) { hi = x - (neg ? -LN2HI : LN2HI); lo = neg ? -LN2LO : LN2LO; k = neg ? -1 : 1; }
    else { k = (int)(u2f(0x3fb8aa3bu) * x + (neg ? -0.5f : 0.5f)); const float t = (float)k; hi = x - t * LN2HI; lo = t * LN2LO; }
    r = hi - lo;
  } else if (ax < 0x39000000u) return 1.f + x;
  const float t = r * r;
  const float c = r - t * (u2f(0x3e2aaa8fu) + t * u2f(0xbb355215u));
  if (k == 0) return 1.f - (fdiv(r * c, c - 2.f) - r);
  const float y = 1.f - ((lo - fdiv(r * c, 2.f - c)) - hi);
  if (k >= -125) { if (k == 128) return (y * 2.f) * u2f(0x7f000000u); return y * u2f((uint32_t)(0x7f + k) << 23); }
  return (y * u2f((uint32_t)(0x7f + k + 100) << 23)) * u2f(0x0d800000u);
}
inline float pm_logf(float x) {
  uint32_t ix = f2u(x); int k = 0;
  if (ix < 0x00800000u) { x = x * 33554432.f; k = -25; ix = f2u(x); }
  k += (int)(ix >> 23) - 127; ix &= 0x007fffffu;
  const uint32_t i = (ix + 0x4afb20u) & 0x800000u;
  const float m = u2f(ix | (i ^ 0x3f800000u)); k += (int)(i >> 23);
  const float f = m - 1.f, s = fdiv(f, 2.f + f), z = s * s, w = z * z;
  const float t1 = w * (u2f(0x3eccce13u) + w * u2f(0x3e789e26u));
  const float t2 = z * (u2f(0x3f2aaaaau) + w * u2f(0x3e91e9eeu));
  const float R = t2 + t1, hfsq = (0.5f * f) * f, dk = (float)k;
  return dk * u2f(0x3f317180u) - ((hfsq - (s * (hfsq + R) + dk * u2f(0x3717f7d1u))) - f);
}
inline float pm_silu(float x) { return fdiv(x, 1.f + pm_expf(-x)); }
inline float pm_sigmoid(float x) { return fdiv(1.f, 1.f + pm_expf(-x)); }
inline float pm_softplus(float x) {
  if (x > 20.f) return x;
  const float y = pm_expf(x), u = 1.f + y;
  const float c = (y <= 1.f) ? y - (u - 1.f) : 1.f - (u - y);
  return pm_logf(u) + fdiv(c, u);
}
// RMS factor of a row from the per-block sums of squares
inline float rfac(const float* S, float eps) { float s = 0.f; for (int b = 0; b < 10; ++b) s = s + S[b]; return pm_rsqrt(fdiv(s, 5120.f) + eps); }
// a8 tail: buf (already multiplied by the input signs) -> int8 in the GEMM layout [Kc/32][MR][32], one scale per block
inline void a8_tail(float* buf, int8_t* qx, float* alpha, int MR, int tok, int blk, int G) {
  fwht512(buf);
  float amax = 0.f;
  for (int i = 0; i < 512; ++i) { buf[i] = rf16(buf[i] * K512); amax = sycl::fmax(amax, sycl::fabs(buf[i])); }
  const float al = fdiv(amax, 127.f);
  alpha[tok * G + blk] = al;
  for (int i = 0; i < 512; ++i) {
    const float r = al > 0.f ? fdiv(buf[i], al) : 0.f;
    int q = (int)sycl::rint(r); q = q < -127 ? -127 : (q > 127 ? 127 : q);
    qx[((size_t)(blk * 16 + (i >> 5)) * MR + tok) * 32 + (i & 31)] = (int8_t)q;
  }
}
// GEMM result in rotated coordinates: ((float)acc * us [* r]) * tau
inline float yrot(int64_t acc, float us, float tau) { return ((float)acc * us) * tau; }
inline float yrot_n(int64_t acc, float us, float r, float tau) { return (((float)acc * us) * r) * tau; }
}  // namespace yx
