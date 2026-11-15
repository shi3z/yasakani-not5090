// SPDX-License-Identifier: Apache-2.0
// ESIMD (explicit SIMD) versions of the device helpers of kern.hpp.  Every function performs, per lane, exactly the IEEE binary32
// operations of its scalar counterpart in the same order, so results are bit-identical (tests/fast_test.cpp checks each one
// against the host; tests/fast_cmp.cpp checks whole layers against the scalar kernels).
// Rules on Xe2: float + - * and rnde are exact; division and sqrt only through div_ieee / sqrt_ieee; loops with a constant trip
// count are unrolled at compile time (unroll<N>), simd values are never indexed at run time.
#pragma once
#include "gemm.hpp"
#include "kern.hpp"
namespace yx {
namespace fx {
using namespace es;
using vf = simd<float, 16>;
using vu = simd<uint32_t, 16>;
using vi = simd<int, 16>;
using vm = simd_mask<16>;
constexpr int F = 17408, ZN = 6144, CV = 10240;      // MLP width, z / v width, qkv width (same values as in engine.hpp)
#define YXI inline __attribute__((always_inline))
YXI vu bits(vf x) { return x.template bit_cast_view<uint32_t>(); }
YXI vf flt(vu u) { return u.template bit_cast_view<float>(); }
YXI vf vdiv(vf a, vf b) { return div_ieee<float, 16>(a, b); }
YXI vf vsqrt(vf a) { return sqrt_ieee<float, 16>(a); }
YXI vf v_rsqrt(vf x) { return vdiv(vf(1.f), vsqrt(x)); }
YXI vu v_f2bf(vf f) { vu u = bits(f); u += 0x7fffu + ((u >> 16) & 1u); return u >> 16; }
YXI vf v_rbf(vf f) { return flt(v_f2bf(f) << 16); }
YXI vf v_bf2f(simd<uint16_t, 16> h) { vu u = h; return flt(u << 16); }
YXI vf v_rf16(vf f) {
  const vu u = bits(f), sign = u & 0x80000000u, a = u & 0x7fffffffu;
  vu res = sign | ((a + 0xfffu + ((a >> 13) & 1u)) & 0xffffe000u);
  const vf r = rnde<float, 16>(flt(a) * 16777216.f);
  res.merge(sign | bits(r * 5.9604644775390625e-08f), a < 0x38800000u);
  res.merge(sign | 0x7f800000u, a >= 0x477ff000u);
  res.merge(u, a >= 0x7f800000u);
  return flt(res);
}
// pm_expf: the "mid" branch of the scalar code is the general branch with k = +-1 (t * LN2 is exact there), and the k == 0
// branch is reached exactly when |x| <= ln2/2 (then hi = x, lo = 0, r = x).
YXI vf v_expf(vf x) {
  const vu ux = bits(x), ax = ux & 0x7fffffffu; const vm neg = ux >= 0x80000000u, big = ax > 0x3eb17218u;
  const float LN2HI = u2f(0x3f317200u), LN2LO = u2f(0x35bfbe8eu);
  vi k = u2f(0x3fb8aa3bu) * x + flt((ux & 0x80000000u) | 0x3f000000u);        // truncating conversion
  vi k1 = 1; k1.merge(vi(-1), neg); k.merge(k1, ax < 0x3f851592u); k.merge(vi(0), !big);
  const vf t = k, hi = x - t * LN2HI, lo = t * LN2LO, r = hi - lo;
  const vf tt = r * r, c = r - tt * (u2f(0x3e2aaa8fu) + tt * u2f(0xbb355215u));
  const vm kz = k == 0;
  vf den = 2.f - c; den.merge(c - 2.f, kz);
  const vf qn = vdiv(r * c, den);
  const vf y = 1.f - ((lo - qn) - hi);
  vf res = y * flt(vu(k + 0x7f) << 23);
  res.merge((y * 2.f) * u2f(0x7f000000u), k == 128);
  res.merge((y * flt(vu(k + (0x7f + 100)) << 23)) * u2f(0x0d800000u), k < -125);
  res.merge(1.f - (qn - r), kz);
  res.merge(1.f + x, ax < 0x39000000u);
  res.merge(vf(0.f), ux > 0xc2cff1b5u);
  res.merge(vf(u2f(0x7f800000u)), (!neg) & (ax >= 0x42b17218u));
  res.merge(x + x, ax > 0x7f800000u);
  return res;
}
YXI vf v_logf(vf x) {
  vu ix = bits(x); vi k = 0; const vm sub = ix < 0x00800000u;
  ix.merge(bits(x * 33554432.f), sub); k.merge(vi(-25), sub);
  k += vi(ix >> 23) - 127; ix &= 0x007fffffu;
  const vu i = (ix + 0x4afb20u) & 0x800000u;
  const vf m = flt(ix | (i ^ 0x3f800000u)); k += vi(i >> 23);
  const vf f = m - 1.f, s = vdiv(f, 2.f + f), z = s * s, w = z * z;
  const vf t1 = w * (u2f(0x3eccce13u) + w * u2f(0x3e789e26u));
  const vf t2 = z * (u2f(0x3f2aaaaau) + w * u2f(0x3e91e9eeu));
  const vf R = t2 + t1, hfsq = (0.5f * f) * f, dk = k;
  return dk * u2f(0x3f317180u) - ((hfsq - (s * (hfsq + R) + dk * u2f(0x3717f7d1u))) - f);
}
YXI vf v_silu(vf x) { return vdiv(x, 1.f + v_expf(flt(bits(x) ^ 0x80000000u))); }
YXI vf v_sigmoid(vf x) { return vdiv(vf(1.f), 1.f + v_expf(flt(bits(x) ^ 0x80000000u))); }
YXI vf v_softplus(vf x) {
  const vf y = v_expf(x), u = 1.f + y;
  vf c = 1.f - (u - y); c.merge(y - (u - 1.f), y <= 1.f);
  vf res = v_logf(u) + vdiv(c, u); res.merge(x, x > 20.f);
  return res;
}
// exact (float)acc of an int64 vector, see tests/fast_test.cpp
YXI vf v_i2f(simd<int64_t, 16> a) { return vf(a); }

// FWHT of 512 values held as x[r] lane l = element 16 r + l; stage order h = 1, 2, ..., 256, (a + b, a - b) with a = lower index
YXI void v_fwht512(vf* x) {
  unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf& v = x[r];
    { simd<float, 8> a = v.template select<8, 2>(0), b = v.template select<8, 2>(1); v.template select<8, 2>(0) = a + b; v.template select<8, 2>(1) = a - b; }
    { auto m = v.template bit_cast_view<float, 4, 4>(); simd<float, 8> a = m.template select<4, 1, 2, 1>(0, 0), b = m.template select<4, 1, 2, 1>(0, 2);
      m.template select<4, 1, 2, 1>(0, 0) = a + b; m.template select<4, 1, 2, 1>(0, 2) = a - b; }
    { auto m = v.template bit_cast_view<float, 2, 8>(); simd<float, 8> a = m.template select<2, 1, 4, 1>(0, 0), b = m.template select<2, 1, 4, 1>(0, 4);
      m.template select<2, 1, 4, 1>(0, 0) = a + b; m.template select<2, 1, 4, 1>(0, 4) = a - b; }
    { simd<float, 8> a = v.template select<8, 1>(0), b = v.template select<8, 1>(8); v.template select<8, 1>(0) = a + b; v.template select<8, 1>(8) = a - b; } });
  unroll<5>([&](auto si) { constexpr int d = 1 << decltype(si)::value;
    unroll<32>([&](auto pi) { constexpr int p = decltype(pi)::value;
      if constexpr (!(p & d)) { const vf a = x[p], b = x[p | d]; x[p] = a + b; x[p | d] = a - b; } }); });
}
// xor_tree32 of the 32 values (a, b): the xor tree is a pairwise halving reduction because float addition commutes
YXI float v_xtree32(vf a, vf b) {
  vf v = a + b; simd<float, 8> c = v.template select<8, 1>(0) + v.template select<8, 1>(8);
  simd<float, 4> d = c.template select<4, 1>(0) + c.template select<4, 1>(4);
  simd<float, 2> e = d.template select<2, 1>(0) + d.template select<2, 1>(2);
  const float e0 = e[0], e1 = e[1]; return e0 + e1;
}
YXI float v_tree128(const vf* a) { return ((v_xtree32(a[0], a[1]) + v_xtree32(a[2], a[3])) + v_xtree32(a[4], a[5])) + v_xtree32(a[6], a[7]); }
// a8 tail of one 512-block (x already multiplied by the input signs); writes alpha[tok * G + blk] and 512 int8 in the GEMM layout
YXI float v_a8_tail(vf* x, int8_t* qx, float* alpha, int MR, int tok, int blk, int G) {
  v_fwht512(x);
  vf amax = 0.f;
  unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = v_rf16(x[r] * K512); amax = (max)(amax, flt(bits(x[r]) & 0x7fffffffu)); });
  const vf al = vdiv(vf(hmax<float>(amax)), vf(127.f)); const vm pos = al > 0.f;
  const float als = al[0]; alpha[tok * G + blk] = als;
  int8_t* qp = qx + ((size_t)(blk * 16) * MR + tok) * 32;
  unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int8_t, 32> o;
    unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value;
      vf r = 0.f; r.merge(vdiv(x[2 * c + hh], al), pos);
      vi q = rnde<float, 16>(r); q = (max)(q, vi(-127)); q = (min)(q, vi(127));
      o.template select<16, 1>(hh * 16) = simd<int8_t, 16>(q); });
    block_store<int8_t, 32>(qp + (size_t)c * MR * 32, o); });
  return als;
}
YXI void v_load512(vf* x, const float* p) { unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = block_load<float, 16>(p + r * 16); }); }
YXI void v_store512(float* p, const vf* x) { unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; block_store<float, 16>(p + r * 16, x[r]); }); }
// RMS factor of a row (rfac of kern.hpp) as a broadcast vector
YXI vf v_rfac(const float* S, float eps) {
  const vf sv = block_load<float, 16>(S); float s = 0.f;
  unroll<10>([&](auto bi) { const float t = sv[decltype(bi)::value]; s = s + t; });
  return v_rsqrt(vdiv(vf(s), vf(5120.f)) + eps);
}

// ---------------------------------------------------------------------------------------------------------------------------
// Kernels.  One ESIMD thread per (row, 512-block); the block lives in 32 registers.
// The a8 producers are launched with one work-group per row (local size G) so that, after a barrier, thread 0 of the group can
// derive the scales of the next GEMM from the G block scales passed through SLM:  us[tok] and the int32 weights Aw[tok][g]
// (exactly what the "prep" kernel of the scalar path computes); the group of the last row also zeroes Aw of the rows >= M.
YXI void v_scales(uint32_t lid, int G, int tok, int M, int MR, float als, float* us, int32_t* Aw) {
  slm_scalar_store<float>(lid * 4, als);
  barrier();
  if (lid != 0) return;
  vf mx = 0.f; vf a[3];
  unroll<3>([&](auto oi) { constexpr int o = decltype(oi)::value; a[o] = 0.f;
    if (16 * o < G) { const vf t = slm_block_load<float, 16>(o * 64); a[o].merge(t, vi(0, 1) < (G - 16 * o)); mx = (max)(mx, a[o]); } });
  const vf mv = hmax<float>(mx); vf rm = 0.f; rm.merge(vdiv(vf(16777216.f), mv), mv > 0.f);
  const float m0 = mv[0]; us[tok] = m0 * 5.9604644775390625e-08f;
  unroll<3>([&](auto oi) { constexpr int o = decltype(oi)::value;
    if (16 * o < G) { const vi w = rnde<float, 16>(a[o] * rm); scatter<int32_t, 16>(Aw + tok * G + 16 * o, vu(0, 4), w, vi(0, 1) < (G - 16 * o)); } });
  if (tok == M - 1) for (int i = M * G; i < MR * G; ++i) Aw[i] = 0;
}
// (float)acc * us [* r] * tau of one 512-block
YXI void v_yrot(vf* x, const int64_t* acc, const float* tau, vf us) {
  unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = (v_i2f(block_load<int64_t, 16>(acc + r * 16)) * us) * block_load<float, 16>(tau + r * 16); }); }
YXI void v_yrot_n(vf* x, const int64_t* acc, const float* tau, vf us, vf rf) {
  unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = ((v_i2f(block_load<int64_t, 16>(acc + r * 16)) * us) * rf) * block_load<float, 16>(tau + r * 16); }); }

// embedding rows -> h
inline void k_embed(sycl::queue& q, int M, const uint32_t* codes, const float* lut, const float* sc, const float* sg, float* h, const int* ids) {
  q.parallel_for(sycl::range<1>(M * 10), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int tok = it[0] / 10, b = it[0] % 10; const int tid = ids[tok]; const float s = sc[tid];
    vf x[32];
    unroll<2>([&](auto hi) { constexpr int hf = decltype(hi)::value; const uint32_t* w = codes + (size_t)tid * 480 + (2 * b + hf) * 24;
      unroll<8>([&](auto ji) { constexpr int jq = decltype(ji)::value;
        vu a = vu(16 * jq, 1) * 6u; a = a - 6u; if constexpr (jq == 0) a.template select<1, 1>(0) = 0u;
        const vu wi = a >> 5, sh = a & 31u; vu wn = wi + 1u; wn = (min)(wn, vu(23u));
        const vu w0 = gather<uint32_t, 16>(w, wi * 4u), w1 = gather<uint32_t, 16>(w, wn * 4u);
        vu st = w0 >> sh; st.merge(st | (w1 << (32u - sh)), sh > 20u); st &= 0xfffu;
        if constexpr (jq == 0) { vu s0 = (w0 & 63u) << 6; st.template select<1, 1>(0) = s0.template select<1, 1>(0); }
        vf e = gather<float, 16>(lut, st * 8u) * s, o = gather<float, 16>(lut + 1, st * 8u) * s;
        vf& xa = x[hf * 16 + 2 * jq]; vf& xb = x[hf * 16 + 2 * jq + 1];
        xa.template select<8, 2>(0) = e.template select<8, 1>(0); xa.template select<8, 2>(1) = o.template select<8, 1>(0);
        xb.template select<8, 2>(0) = e.template select<8, 1>(8); xb.template select<8, 2>(1) = o.template select<8, 1>(8); }); });
    v_fwht512(x);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
      block_store<float, 16>(h + (size_t)tok * H + b * 512 + r * 16, v_rbf((x[r] * K512) * block_load<float, 16>(sg + b * 512 + r * 16))); }); });
}
// residual add (acc may be null) + per-block sums of squares + quantised input of the next GEMM (+ its scales)
inline void k_res_norm(sycl::queue& q, int M, int MR, float* h, float* S, float* alpha, int8_t* qx, uint16_t* xu, const int64_t* acc, const float* tau, const float* so,
                       float* us, int32_t* Aw, const float* w, const float* sg_in) {
  q.parallel_for(sycl::nd_range<1>(M * 10, 10), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 10, blk = gid % 10; float* hp = h + (size_t)tok * H + blk * 512; vf x[32];
    if (acc) { v_yrot(x, acc + (size_t)tok * H + blk * 512, tau + blk * 512, vf(us[tok]));
      v_fwht512(x);
      unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
        x[r] = v_rbf(block_load<float, 16>(hp + r * 16) + v_rbf((x[r] * K512) * block_load<float, 16>(so + blk * 512 + r * 16))); block_store<float, 16>(hp + r * 16, x[r]); }); }
    else v_load512(x, hp);
    vf a[8];
    unroll<8>([&](auto qi) { constexpr int k = decltype(qi)::value; vf s = 0.f;
      unroll<4>([&](auto ji) { constexpr int j = decltype(ji)::value; s = s + x[k + 8 * j] * x[k + 8 * j]; }); a[k] = s; });
    S[tok * SST + blk] = v_tree128(a);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
      const vf u = x[r] * (1.f + block_load<float, 16>(w + blk * 512 + r * 16));
      if (xu) block_store<uint16_t, 16>(xu + (size_t)tok * H + blk * 512 + r * 16, simd<uint16_t, 16>(v_f2bf(u)));
      x[r] = u * block_load<float, 16>(sg_in + blk * 512 + r * 16); });
    const float als = v_a8_tail(x, qx, alpha, MR, tok, blk, 10);
    v_scales((uint32_t)it.get_local_id(0), 10, tok, M, MR, als, us, Aw); });
}
// GDN, after the qkv / z GEMMs.  128 threads per row: 20 blocks of qkv -> mixed (bf16), 12 blocks of z -> z,
// 96 a / b projections (-> ab, and from them beta and g).
struct GdnPost { const int64_t *aq, *az; const float *tq, *tz, *sq, *sz, *us, *S, *wa, *wb, *A_log, *dt; const uint16_t* xn; uint16_t* mixed; float *z, *ab, *beta, *g; float eps; };
inline void k_gdn_post(sycl::queue& q, int M, const GdnPost P) {
  q.parallel_for(sycl::range<1>(M * 128), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int tok = it[0] >> 7, b = it[0] & 127;
    const vf rf = v_rfac(P.S + tok * SST, P.eps);
    if (b < 32) { vf x[32];
      if (b < 20) { v_yrot_n(x, P.aq + (size_t)tok * CV + b * 512, P.tq + b * 512, vf(P.us[tok]), rf); v_fwht512(x);
        unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
          block_store<uint16_t, 16>(P.mixed + (size_t)tok * CV + b * 512 + r * 16, simd<uint16_t, 16>(v_f2bf((x[r] * K512) * block_load<float, 16>(P.sq + b * 512 + r * 16)))); }); }
      else { const int bb = b - 20; v_yrot_n(x, P.az + (size_t)tok * ZN + bb * 512, P.tz + bb * 512, vf(P.us[tok]), rf); v_fwht512(x);
        unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
          block_store<float, 16>(P.z + (size_t)tok * ZN + bb * 512 + r * 16, v_rbf((x[r] * K512) * block_load<float, 16>(P.sz + bb * 512 + r * 16))); }); }
      return; }
    const int r = b - 32; const float* W = r < 48 ? P.wb + (size_t)r * H : P.wa + (size_t)(r - 48) * H; const uint16_t* X = P.xn + (size_t)tok * H;
    vf a[8]; unroll<8>([&](auto qi) { a[decltype(qi)::value] = 0.f; });
    for (int j = 0; j < 5; ++j)
      unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; const int i0 = 8 * (16 * k8 + 128 * j);
        simd<float, 128> wv; simd<uint16_t, 128> xv;
        unroll<2>([&](auto pi) { constexpr int p = decltype(pi)::value;
          wv.template select<64, 1>(64 * p) = block_load<float, 64>(W + i0 + 64 * p); xv.template select<64, 1>(64 * p) = block_load<uint16_t, 64>(X + i0 + 64 * p); });
        unroll<8>([&](auto ki) { constexpr int k = decltype(ki)::value;
          const vf wk = wv.template select<16, 8>(k); const simd<uint16_t, 16> xk = xv.template select<16, 8>(k);
          a[k8] = a[k8] + wk * v_bf2f(xk); }); });
    const vf abv = v_rbf(vf(v_tree128(a)) * rf); const float ab0 = abv[0]; P.ab[tok * 96 + r] = ab0;
    if (r < 48) { const vf bt = v_rbf(v_sigmoid(abv)); const float b0 = bt[0]; P.beta[tok * 48 + r] = b0; }
    else { const int hd = r - 48; const vf e = v_expf(vf(P.A_log[hd]));
      const vf gg = flt(bits(e) ^ 0x80000000u) * v_softplus(abv + P.dt[hd]); const float g0 = gg[0]; P.g[tok * 48 + hd] = g0; } });
}
// causal conv (4 taps over rows t-3..t, history for rows < 0) + silu; q / k slots are L2-normalised
inline void k_conv(sycl::queue& q, int M, const uint16_t* mixed, const float* cw, const float* hist, float* Q, float* K, float* V) {
  q.parallel_for(sycl::range<1>(M * 80), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int t = it[0] / 80, slot = it[0] % 80; vf act[8];
    unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; const int c = slot * 128 + 16 * k8;
      simd<float, 64> cv = block_load<float, 64>(cw + (size_t)c * 4); vf x[4];
      unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; const int r = t - 3 + k;
        if (r >= 0) x[k] = v_bf2f(block_load<uint16_t, 16>(mixed + (size_t)r * CV + c)); else x[k] = block_load<float, 16>(hist + (size_t)(r + 3) * CV + c); });
      const vf c0 = cv.template select<16, 4>(0), c1 = cv.template select<16, 4>(1), c2 = cv.template select<16, 4>(2), c3 = cv.template select<16, 4>(3);
      const vf acc = ((c0 * x[0] + c1 * x[1]) + c2 * x[2]) + c3 * x[3];
      act[k8] = v_rbf(v_silu(v_rbf(acc))); });
    if (slot < 32) { vf sq[8]; unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; sq[k8] = act[k8] * act[k8]; });
      const vf rr = v_rsqrt(vf(v_tree128(sq) + 1e-6f));
      if (slot < 16) unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; block_store<float, 16>(Q + (size_t)t * 2048 + slot * 128 + k8 * 16, (act[k8] * rr) * 0.08838834764831845f); });
      else unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; block_store<float, 16>(K + (size_t)t * 2048 + (slot - 16) * 128 + k8 * 16, act[k8] * rr); }); }
    else unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; block_store<float, 16>(V + (size_t)t * ZN + (slot - 32) * 128 + k8 * 16, act[k8]); }); });
}
// State recurrence over the M rows: one thread per (head, 16 value columns j); the 128 key rows i are walked in order inside
// each of the 4 chunks of 32.  Rows < n_commit update Sp in place; later rows continue in the scratch copy Ss.
// Threads >= 384 (only when committing) roll the conv history: 20 blocks of 512 columns.
inline void k_state(sycl::queue& q, int M, int n_commit, const float* Q, const float* K, const float* V, const float* B, const float* Gg, float* Sp, float* Ss, float* core,
                    const uint16_t* mixed, float* hist) {
  // The recurrence is serial in the rows, so its latency per row is what counts.  A (head, 16 state columns) block is shared by a
  // work-group of 2 threads; each keeps 64 of the 128 state rows in REGISTERS (no memory traffic for the state) and the two exchange
  // their partial sums through SLM with two barriers per row.  Row t: s *= e^g, kv = sum_i s_i k_i, d = (v - kv) * beta, s += k d,
  // o = sum_i s_i q_i -- per element the same operations in the same order as before (4 partial sums of 32 rows, ((p0+p1)+p2)+p3).
  // The state after row n_commit - 1 is written back to Sp; later rows (look-ahead) only change the registers.  Ss is no longer used.
  (void)Ss;
  if (n_commit > 0) q.parallel_for(sycl::range<1>(20), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int c0 = (int)it[0] * 512;       // conv history: the last 3 committed rows
      unroll<32>([&](auto ri) { constexpr int r16 = decltype(ri)::value * 16; vf nw[3];
        unroll<3>([&](auto ki) { constexpr int k = decltype(ki)::value; const int r = n_commit - 3 + k;
          if (r >= 0) nw[k] = v_bf2f(block_load<uint16_t, 16>(mixed + (size_t)r * CV + c0 + r16)); else nw[k] = block_load<float, 16>(hist + (size_t)(3 + r) * CV + c0 + r16); });
        unroll<3>([&](auto ki) { constexpr int k = decltype(ki)::value; block_store<float, 16>(hist + (size_t)k * CV + c0 + r16, nw[k]); }); }); });
  q.parallel_for(sycl::nd_range<1>(768, 2), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    slm_init<512>();
    const int sub = (int)ndi.get_local_id(0), blk = (int)ndi.get_group(0), hd = blk >> 3, j0 = (blk & 7) * 16, kh = hd / 3, i0 = 64 * sub;
    const size_t so = (size_t)hd * 16384 + j0;
    vf s[64]; unroll<64>([&](auto ii) { constexpr int i = decltype(ii)::value; s[i] = block_load<float, 16>(Sp + so + (size_t)(i0 + i) * 128); });
    for (int t = 0; t < M; ++t) { const float* kt = K + (size_t)t * 2048 + kh * 128 + i0; const float* qt = Q + (size_t)t * 2048 + kh * 128 + i0;
      const vf eg = v_expf(vf(Gg[t * 48 + hd])); vf kk[4], a[2];
      unroll<4>([&](auto xi) { constexpr int x = decltype(xi)::value; kk[x] = block_load<float, 16>(kt + 16 * x); });
      unroll<2>([&](auto wi) { constexpr int w = decltype(wi)::value; vf acc = 0.f;
        unroll<32>([&](auto li) { constexpr int i = 32 * w + decltype(li)::value; s[i] = s[i] * eg; const float k1 = kk[i / 16][i % 16]; acc = acc + s[i] * k1; });
        a[w] = acc; });
      slm_block_store<float, 16>(sub * 128, a[0]); slm_block_store<float, 16>(sub * 128 + 64, a[1]);
      barrier();
      const vf kv0 = slm_block_load<float, 16>(0), kv1 = slm_block_load<float, 16>(64), kv2 = slm_block_load<float, 16>(128), kv3 = slm_block_load<float, 16>(192);
      const vf d = (block_load<float, 16>(V + (size_t)t * ZN + hd * 128 + j0) - (((kv0 + kv1) + kv2) + kv3)) * B[t * 48 + hd];
      vf qq[4]; unroll<4>([&](auto xi) { constexpr int x = decltype(xi)::value; qq[x] = block_load<float, 16>(qt + 16 * x); });
      unroll<2>([&](auto wi) { constexpr int w = decltype(wi)::value; vf acc = 0.f;
        unroll<32>([&](auto li) { constexpr int i = 32 * w + decltype(li)::value; const float k1 = kk[i / 16][i % 16], q1 = qq[i / 16][i % 16]; s[i] = s[i] + k1 * d; acc = acc + s[i] * q1; });
        a[w] = acc; });
      slm_block_store<float, 16>(256 + sub * 128, a[0]); slm_block_store<float, 16>(256 + sub * 128 + 64, a[1]);
      barrier();
      if (sub == 0) { const vf o0 = slm_block_load<float, 16>(256), o1 = slm_block_load<float, 16>(320), o2 = slm_block_load<float, 16>(384), o3 = slm_block_load<float, 16>(448);
        block_store<float, 16>(core + (size_t)t * ZN + hd * 128 + j0, v_rbf(((o0 + o1) + o2) + o3)); }
      if (t + 1 == n_commit) unroll<64>([&](auto ii) { constexpr int i = decltype(ii)::value; block_store<float, 16>(Sp + so + (size_t)(i0 + i) * 128, s[i]); }); } });
}
// per-head RMS norm of core, gate with silu(z), quantised input of out_proj (+ its scales)
inline void k_gdn_out(sycl::queue& q, int M, int MR, const float* core, const float* z, const float* gw, const float* si, float eps, float* alpha, int8_t* qx, float* us, int32_t* Aw) {
  q.parallel_for(sycl::nd_range<1>(M * 12, 12), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 12, blk = gid % 12; vf x[32]; v_load512(x, core + (size_t)tok * ZN + blk * 512);
    unroll<4>([&](auto hi) { constexpr int hd = decltype(hi)::value; vf sq[8];
      unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; sq[k8] = x[hd * 8 + k8] * x[hd * 8 + k8]; });
      const vf rh = v_rsqrt(vdiv(vf(v_tree128(sq)), vf(128.f)) + eps);
      unroll<8>([&](auto qi) { constexpr int k8 = decltype(qi)::value; constexpr int r = hd * 8 + k8;
        const vf n2 = v_rbf(block_load<float, 16>(gw + k8 * 16) * v_rbf(x[r] * rh));
        x[r] = v_rbf(n2 * v_silu(block_load<float, 16>(z + (size_t)tok * ZN + blk * 512 + r * 16))) * block_load<float, 16>(si + blk * 512 + r * 16); }); });
    const float als = v_a8_tail(x, qx, alpha, MR, tok, blk, 12);
    v_scales((uint32_t)it.get_local_id(0), 12, tok, M, MR, als, us, Aw); });
}
// MLP after the gate / up GEMMs: silu(gate) * up -> quantised input of down_proj (+ its scales)
struct MlpPost { const int64_t *ag, *au; const float *tg, *tu, *sg, *su, *si, *S; float *us, *alpha; int8_t* qx; int32_t* Aw; float eps; };
inline void k_mlp_post(sycl::queue& q, int M, int MR, const MlpPost P) {
  q.parallel_for(sycl::nd_range<1>(M * 34, 34), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 34, blk = gid % 34; const size_t n0 = (size_t)tok * F + blk * 512;
    const vf rf = v_rfac(P.S + tok * SST, P.eps), u0 = P.us[tok]; vf x[32], y[32];
    v_yrot_n(y, P.au + n0, P.tu + blk * 512, u0, rf); v_fwht512(y);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; y[r] = v_rbf((y[r] * K512) * block_load<float, 16>(P.su + blk * 512 + r * 16)); });
    v_yrot_n(x, P.ag + n0, P.tg + blk * 512, u0, rf); v_fwht512(x);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
      const vf g = v_rbf((x[r] * K512) * block_load<float, 16>(P.sg + blk * 512 + r * 16));
      x[r] = v_rbf(v_rbf(v_silu(g)) * y[r]) * block_load<float, 16>(P.si + blk * 512 + r * 16); });
    const float als = v_a8_tail(x, P.qx, P.alpha, MR, tok, blk, 34);
    v_scales((uint32_t)it.get_local_id(0), 34, tok, M, MR, als, P.us, P.Aw); });
}
// true if the device contracts a * b + c into a fused multiply-add inside ESIMD kernels (it must not: build.sh passes
// -Xsycl-target-backend "-ffp-contract=off"; without it results are off by one ulp here and there)
inline bool esimd_contracts(sycl::queue& q) {
  float* d = sycl::malloc_device<float>(64, q); float in[64] = {}; for (int i = 0; i < 16; ++i) { in[i] = 1.000244140625f; in[16 + i] = 1.000244140625f; in[32 + i] = -1.00048828125f; }
  q.memcpy(d, in, sizeof in).wait();
  q.parallel_for(sycl::range<1>(1), [=](sycl::id<1>) SYCL_ESIMD_KERNEL {
    block_store<float, 16>(d + 48, block_load<float, 16>(d) * block_load<float, 16>(d + 16) + block_load<float, 16>(d + 32)); }).wait();
  q.memcpy(in, d, sizeof in).wait(); sycl::free(d, q); return in[48] != 0.f;
}
}  // namespace fx
}  // namespace yx
