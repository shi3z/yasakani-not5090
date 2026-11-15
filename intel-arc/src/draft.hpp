// SPDX-License-Identifier: Apache-2.0
// The draft model "bdraft2" (spec/D_spec_api.md section 3) for Intel Xe2 / SYCL: context path with its ring buffers, the one-shot
// draft block (8 or 16 rows), the SHEAD output head and the host-side candidate selection (greedy and Gumbel branch).
// The drafter only proposes tokens: its arithmetic is deterministic but not bit-identical to the CUDA engine.
// Not ported: HCAP side candidates / row tables / leaf promotion (spec D 3.5) -> hcap_on() is false.
#pragma once
#include "engine.hpp"
#include "head.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace yx {
namespace dx {
using namespace fx;
YXI float v_hsum(vf v) {
  simd<float, 8> c = v.template select<8, 1>(0) + v.template select<8, 1>(8);
  simd<float, 4> d = c.template select<4, 1>(0) + c.template select<4, 1>(4);
  simd<float, 2> e = d.template select<2, 1>(0) + d.template select<2, 1>(2);
  const float e0 = e[0], e1 = e[1]; return e0 + e1;
}
constexpr float NINF = -__builtin_inff();
// ---- RMS norm (+ optional residual update with the 2-tap dynamic conv, or a GEMM result as the input) -> x1, a8, scales ----
// mode 0: x = h      mode 1: h += (b0 + d0) * y[m] + (b1 + d1) * y[m-1], x = h      mode 2: x = y  (y = (float)acc * us * tau)
struct NormA { int mode; float* h; const int64_t* acc; const float *tau, *dyn, *base, *us_in, *w, *sg; float* x1; int8_t* qx; float *alpha, *us_out; int32_t* Aw; float eps; };
inline void k_norm(sycl::queue& q, int R, int MR, const NormA P) {
  q.parallel_for(sycl::nd_range<1>(R * 10, 10), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<1024>();
    const int gid = (int)it.get_global_id(0), tok = gid / 10, blk = gid % 10, o = blk * 512; const uint32_t lid = (uint32_t)it.get_local_id(0);
    vf x[32];
    if (P.mode == 0) v_load512(x, P.h + (size_t)tok * H + o);
    else if (P.mode == 2) v_yrot(x, P.acc + (size_t)tok * H + o, P.tau + o, vf(P.us_in[tok]));
    else {
      float* hp = P.h + (size_t)tok * H + o; const int tp = tok > 0 ? tok - 1 : 0;
      const float* dy = P.dyn + (size_t)tok * 1280 + 640 + blk * 32;
      vf d[4]; unroll<4>([&](auto si) { constexpr int s = decltype(si)::value; d[s] = block_load<float, 16>(dy + (s / 2) * 320 + (s % 2) * 16); });
      const vf u0 = P.us_in[tok], u1 = tok > 0 ? P.us_in[tp] : 0.f;
      const int64_t *a0 = P.acc + (size_t)tok * H + o, *a1 = P.acc + (size_t)tp * H + o;
      unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
        const vf t = block_load<float, 16>(P.tau + o + r * 16);
        const vf y0 = (v_i2f(block_load<int64_t, 16>(a0 + r * 16)) * u0) * t, y1 = (v_i2f(block_load<int64_t, 16>(a1 + r * 16)) * u1) * t;
        const float g0 = d[r / 16][r % 16], g1 = d[2 + r / 16][r % 16];
        x[r] = block_load<float, 16>(hp + r * 16) + ((block_load<float, 16>(P.base + o + r * 16) + g0) * y0 + (block_load<float, 16>(P.base + H + o + r * 16) + g1) * y1);
        block_store<float, 16>(hp + r * 16, x[r]); });
    }
    vf s = 0.f; unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; s = s + x[r] * x[r]; });
    slm_scalar_store<float>(512 + lid * 4, v_hsum(s)); barrier();
    const vf sv = slm_block_load<float, 16>(512); float tot = 0.f;
    unroll<10>([&](auto bi) { const float t = sv[decltype(bi)::value]; tot = tot + t; });
    const vf rr = v_rsqrt(vdiv(vf(tot), vf(5120.f)) + P.eps);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
      const vf u = (rr * x[r]) * block_load<float, 16>(P.w + o + r * 16);
      if (P.x1) block_store<float, 16>(P.x1 + (size_t)tok * H + o + r * 16, u);
      x[r] = u * block_load<float, 16>(P.sg + o + r * 16); });
    const float als = v_a8_tail(x, P.qx, P.alpha, MR, tok, blk, 10);
    v_scales(lid, 10, tok, R, MR, als, P.us_out, P.Aw); });
}
// ---- gains from the akp / mkp GEMM, 2-tap causal dynamic conv over the rows, a8 ----
struct DynA { const int64_t* acc; const float *tau, *us_in, *x1, *base, *sg; float* dyn; int8_t* qx; float *alpha, *us_out; int32_t* Aw; };
inline void k_dyn(sycl::queue& q, int R, int MR, const DynA P) {
  q.parallel_for(sycl::nd_range<1>(R * 10, 10), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 10, c = gid % 10, o = c * 512; const vf u = P.us_in[tok];
    vf d[8];
    unroll<8>([&](auto si) { constexpr int s = decltype(si)::value; const int idx = (s / 2) * 320 + c * 32 + (s % 2) * 16;
      d[s] = (v_i2f(block_load<int64_t, 16>(P.acc + (size_t)tok * 1280 + idx)) * u) * block_load<float, 16>(P.tau + idx);
      block_store<float, 16>(P.dyn + (size_t)tok * 1280 + idx, d[s]); });
    const float *xa = P.x1 + (size_t)tok * H + o, *xb = P.x1 + (size_t)(tok > 0 ? tok - 1 : 0) * H + o; const float pm = tok > 0 ? 1.f : 0.f;
    vf x[32];
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value;
      const float g0 = d[r / 16][r % 16], g1 = d[2 + r / 16][r % 16];
      const vf v = (block_load<float, 16>(P.base + o + r * 16) + g0) * block_load<float, 16>(xa + r * 16)
                 + ((block_load<float, 16>(P.base + H + o + r * 16) + g1) * block_load<float, 16>(xb + r * 16)) * pm;
      x[r] = v * block_load<float, 16>(P.sg + o + r * 16); });
    const float als = v_a8_tail(x, P.qx, P.alpha, MR, tok, c, 10);
    v_scales((uint32_t)it.get_local_id(0), 10, tok, R, MR, als, P.us_out, P.Aw); });
}
// ---- GLU: silu(gate) * up -> a8 (34 groups) ----
struct GluA { const int64_t *ag, *au; const float *tg, *tu, *us_in, *sg; int8_t* qx; float *alpha, *us_out; int32_t* Aw; };
inline void k_glu(sycl::queue& q, int R, int MR, const GluA P) {
  q.parallel_for(sycl::nd_range<1>(R * 34, 34), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 34, blk = gid % 34; const size_t n0 = (size_t)tok * yx::F + blk * 512; const vf u0 = P.us_in[tok];
    vf x[32], y[32];
    v_yrot(y, P.au + n0, P.tu + blk * 512, u0); v_yrot(x, P.ag + n0, P.tg + blk * 512, u0);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = (v_silu(x[r]) * y[r]) * block_load<float, 16>(P.sg + blk * 512 + r * 16); });
    const float als = v_a8_tail(x, P.qx, P.alpha, MR, tok, blk, 34);
    v_scales((uint32_t)it.get_local_id(0), 34, tok, R, MR, als, P.us_out, P.Aw); });
}
// ---- a8 of the captured hidden states: 50 groups, group g = capture g / 10, block g % 10 (scales by k_prep) ----
inline void k_a8cap(sycl::queue& q, int n, int MR, const float* cap, const float* sg, int8_t* qx, float* alpha) {
  q.parallel_for(sycl::range<1>(n * 50), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL {
    const int tok = (int)it[0] / 50, g = (int)it[0] % 50; const float* src = cap + ((size_t)(g / 10) * MRMAX + tok) * H + (g % 10) * 512; vf x[32];
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = block_load<float, 16>(src + r * 16) * block_load<float, 16>(sg + g * 512 + r * 16); });
    v_a8_tail(x, qx, alpha, MR, tok, g, 50); });
}
inline void k_prep(sycl::queue& q, int M, int MR, int G, const float* al, float* us, int32_t* aw) {
  q.parallel_for(sycl::range<1>(MR), [=](sycl::id<1> id) { const int tok = id[0];
    if (tok >= M) { for (int g = 0; g < G; ++g) aw[tok * G + g] = 0; return; }
    float m = 0.f; for (int g = 0; g < G; ++g) m = sycl::fmax(m, al[tok * G + g]);
    const float rm = m > 0.f ? 16777216.f / m : 0.f; us[tok] = m * 5.9604644775390625e-08f;
    for (int g = 0; g < G; ++g) aw[tok * G + g] = (int32_t)sycl::rint(al[tok * G + g] * rm); });
}
// ---- XMX (bf16 x bf16 -> float) layouts of the attention keys and values, per KV head and group of 16 keys (uint16 index):
//   keys:   [dim chunk c of 8][dim pair k of 8][key lane 16][2]   = dims 16 c + 2 k, 16 c + 2 k + 1 of key `lane`
//   values: [dim chunk c of 8][key pair k of 8][dim lane 16][2]   = dim 16 c + lane of keys 2 k, 2 k + 1
inline size_t kix(int key, int d) { return ((size_t)(((d >> 4) * 8 + ((d & 15) >> 1)) * 16 + key) << 1) + (d & 1); }
inline size_t vix(int key, int d) { return ((size_t)(((d >> 4) * 8 + (key >> 1)) * 16 + (d & 15)) << 1) + (key & 1); }
constexpr int KVG = 2048;      // uint16 per (KV head, group of 16 keys)
// ---- q / k: per-head RMS norm + rotary position (table row pos: cos[64], sin[64]);  v: plain.  q -> yq (float), the block's own
// k / v rows -> bk / bv in the XMX layouts [8 heads][KVG] (key lane = block row) ----
struct QkvA { const int64_t *aq, *ak, *av; const float *tq, *tk, *tv, *us, *qn, *kn, *rope; float* yq; uint16_t *bk, *bv; float eps; int p; };
inline void k_qkv(sycl::queue& q, int R, const QkvA P) {
  q.parallel_for(sycl::range<1>(R * 48), [=](sycl::id<1> it) { const int tok = (int)it[0] / 48, hh = (int)it[0] % 48; const float u = P.us[tok];
    if (hh >= 40) { const int h8 = hh - 40, o = h8 * 128; for (int d = 0; d < 128; ++d) P.bv[(size_t)h8 * KVG + vix(tok, d)] = f2bf(((float)P.av[(size_t)tok * 1024 + o + d] * u) * P.tv[o + d]); return; }
    const bool isq = hh < 32; const int o = (isq ? hh : hh - 32) * 128;
    const int64_t* a = isq ? P.aq + (size_t)tok * 4096 + o : P.ak + (size_t)tok * 1024 + o; const float* t = (isq ? P.tq : P.tk) + o; const float* w = isq ? P.qn : P.kn;
    float v[128], ss = 0.f; for (int d = 0; d < 128; ++d) { v[d] = ((float)a[d] * u) * t[d]; ss += v[d] * v[d]; }
    const float rr = 1.f / sycl::sqrt(ss / 128.f + P.eps); for (int d = 0; d < 128; ++d) v[d] = rr * v[d] * w[d];
    const float* rt = P.rope + (size_t)(P.p + tok) * 128;
    if (isq) { float* out = P.yq + (size_t)tok * 4096 + o; for (int d = 0; d < 64; ++d) { const float c = rt[d], s = rt[64 + d]; out[d] = v[d] * c - v[d + 64] * s; out[d + 64] = v[d + 64] * c + v[d] * s; } }
    else { uint16_t* out = P.bk + (size_t)(hh - 32) * KVG;
      for (int d = 0; d < 64; ++d) { const float c = rt[d], s = rt[64 + d]; out[kix(tok, d)] = f2bf(v[d] * c - v[d + 64] * s); out[kix(tok, d + 64)] = f2bf(v[d + 64] * c + v[d] * s); } } });
}
// ---- context rows -> rings of the 5 layers (k: norm + rope, v: plain; bf16; position t sits in key group (t & 2047) >> 4, lane t & 15) ----
// rings: [5 layers][8 heads][128 groups][KVG] uint16
struct RingA { const int64_t *ak[5], *av[5]; const float *tk[5], *tv[5], *kn[5]; const float *us, *rope; uint16_t *rk, *rv; float eps; int p; };
inline void k_ring(sycl::queue& q, int n, const RingA P) {
  q.parallel_for(sycl::range<1>(n * 80), [=](sycl::id<1> it) { const int tok = (int)it[0] / 80, r = (int)it[0] % 80, i = r / 16, hh = r % 16, h8 = hh & 7, o = h8 * 128;
    const float u = P.us[tok]; const int slot = (P.p + tok) & 2047, key = slot & 15; const size_t dst = (((size_t)i * 8 + h8) * 128 + (slot >> 4)) * KVG;
    if (hh >= 8) { for (int d = 0; d < 128; ++d) P.rv[dst + vix(key, d)] = f2bf(((float)P.av[i][(size_t)tok * 1024 + o + d] * u) * P.tv[i][o + d]); return; }
    float v[128], ss = 0.f; for (int d = 0; d < 128; ++d) { v[d] = ((float)P.ak[i][(size_t)tok * 1024 + o + d] * u) * P.tk[i][o + d]; ss += v[d] * v[d]; }
    const float rr = 1.f / sycl::sqrt(ss / 128.f + P.eps); for (int d = 0; d < 128; ++d) v[d] = rr * v[d] * P.kn[i][d];
    const float* rt = P.rope + (size_t)(P.p + tok) * 128;
    for (int d = 0; d < 64; ++d) { const float c = rt[d], s = rt[64 + d]; P.rk[dst + kix(key, d)] = f2bf(v[d] * c - v[d + 64] * s); P.rk[dst + kix(key, d + 64)] = f2bf(v[d + 64] * c + v[d] * s); } });
}
// ---- attention of the block rows: ring entries [max(lo, p + m - 2047), p) of the layer + all R block rows ----
// One thread per (part, row, pair of query heads).  Per group of 16 keys: scores = 8 XMX products (queries as bf16), running-maximum
// softmax, weighted values = 8 XMX products (weights as bf16).  The key groups of a row are split into NP parts (the block rows go
// to the last one); k_a8att merges the parts.   part layout: [NP][R * 32][144] floats: [0] max, [1] sum, [16..144) values
// one group of 16 keys for 2 query heads: A = the queries (bf16, [dim chunk][head][8 dwords]), o = the weighted values [dim chunk][head][16]
YXI void att_group(simd<int, 16>* A, simd<float, 32>* o, float* mx, float* den, const int* KB, const int* VB, simd_mask<16> bad) {
  constexpr auto BF = xmx::dpas_argument_type::bf16;
  simd<float, 32> acc = 0.f;
  unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
    B.template select<64, 1>(0) = block_load<int, 64>(KB + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(KB + c * 128 + 64);
    acc = xmx::dpas<8, 2, float, float, int, int, BF, BF>(acc, B, A[c]); });
  vf s0 = acc.template select<16, 1>(0), s1 = acc.template select<16, 1>(16); s0.merge(vf(NINF), bad); s1.merge(vf(NINF), bad);
  const float g0 = hmax<float>(s0), g1 = hmax<float>(s1), n0 = g0 > mx[0] ? g0 : mx[0], n1 = g1 > mx[1] ? g1 : mx[1];
  vf cd = 0.f; cd.template select<1, 1>(0) = mx[0] - n0; cd.template select<1, 1>(1) = mx[1] - n1; cd = v_expf(cd); const float c0 = cd[0], c1 = cd[1];
  const vf e0 = v_expf(s0 - n0), e1 = v_expf(s1 - n1);
  den[0] = den[0] * c0 + v_hsum(e0); den[1] = den[1] * c1 + v_hsum(e1); mx[0] = n0; mx[1] = n1;
  simd<float, 32> cf; cf.template select<16, 1>(0) = vf(c0); cf.template select<16, 1>(16) = vf(c1);
  simd<int, 16> Ae; { vu b = v_f2bf(e0); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); Ae.template select<8, 1>(0) = simd<int, 8>(lo | (hi << 16)); }
  { vu b = v_f2bf(e1); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); Ae.template select<8, 1>(8) = simd<int, 8>(lo | (hi << 16)); }
  unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
    B.template select<64, 1>(0) = block_load<int, 64>(VB + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(VB + c * 128 + 64);
    o[c] = xmx::dpas<8, 2, float, float, int, int, BF, BF>(o[c] * cf, B, Ae); });
}
struct AttA { const float* yq; const uint16_t *bk, *bv, *rk, *rv; float* part; int p, lo, R, NP; };
inline void k_attn(sycl::queue& q, const AttA P) {
  q.parallel_for(sycl::range<1>((size_t)P.NP * P.R * 16), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL {
    const int rp = (int)it[0] % (P.R * 16), sp = (int)it[0] / (P.R * 16), tok = rp >> 4, pr = rp & 15, g = pr >> 1;
    simd<int, 16> A[8]; simd<float, 32> o[8]; const float* qp = P.yq + ((size_t)tok * 32 + pr * 2) * 128;
    unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; o[c] = 0.f;
      unroll<2>([&](auto mi) { constexpr int m = decltype(mi)::value; vu b = v_f2bf(block_load<float, 16>(qp + m * 128 + 16 * c) * 0.08838834764831845f);
        const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); A[c].template select<8, 1>(8 * m) = simd<int, 8>(lo | (hi << 16)); }); });
    float mx[2] = {NINF, NINF}, den[2] = {0.f, 0.f};
    int t0 = P.p + tok - 2047; if (t0 < 0) t0 = 0; if (t0 < P.lo) t0 = P.lo;
    if (P.p > t0) { const int ga = t0 >> 4, ng = ((P.p - 1) >> 4) - ga + 1, ch = (ng + P.NP - 1) / P.NP, g0 = ga + sp * ch, g1 = g0 + ch < ga + ng ? g0 + ch : ga + ng;
      const int *RK = (const int*)(P.rk + (size_t)g * 128 * KVG), *RV = (const int*)(P.rv + (size_t)g * 128 * KVG);
      for (int tg = g0; tg < g1; ++tg) { const vi tv(tg * 16, 1); const size_t off = (size_t)(tg & 127) * (KVG / 2);
        att_group(A, o, mx, den, RK + off, RV + off, (tv < t0) | (tv >= P.p)); } }
    if (sp == P.NP - 1) att_group(A, o, mx, den, (const int*)(P.bk + (size_t)g * KVG), (const int*)(P.bv + (size_t)g * KVG), vi(0, 1) >= P.R);
    unroll<2>([&](auto mi) { constexpr int m = decltype(mi)::value; float* op = P.part + ((size_t)sp * P.R * 32 + tok * 32 + pr * 2 + m) * 144;
      vf hd = 0.f; hd.template select<1, 1>(0) = mx[m]; hd.template select<1, 1>(1) = den[m]; block_store<float, 16>(op, hd);
      unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; const vf x = o[c].template select<16, 1>(16 * m); block_store<float, 16>(op + 16 + 16 * c, x); }); }); });
}
// merge the attention parts of the 4 heads of a 512-block, a8 (8 groups), scales
inline void k_a8att(sycl::queue& q, int R, int MR, int NP, const float* part, const float* sg, int8_t* qx, float* alpha, float* us_out, int32_t* Aw) {
  q.parallel_for(sycl::nd_range<1>(R * 8, 8), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 8, blk = gid % 8; vf x[32];
    unroll<4>([&](auto hi) { constexpr int hd = decltype(hi)::value; const size_t rh = (size_t)tok * 32 + blk * 4 + hd;
      float mx = NINF; for (int s = 0; s < NP; ++s) { const float m = part[((size_t)s * R * 32 + rh) * 144]; mx = m > mx ? m : mx; }
      vf o[8]; unroll<8>([&](auto ki) { o[decltype(ki)::value] = 0.f; }); float den = 0.f;
      for (int s = 0; s < NP; ++s) { const float* pp = part + ((size_t)s * R * 32 + rh) * 144; const float ms = pp[0];
        if (ms > NINF) { const vf c = v_expf(vf(ms - mx)); const float c0 = c[0]; den = den + pp[1] * c0;
          unroll<8>([&](auto ki) { constexpr int k = decltype(ki)::value; o[k] = o[k] + block_load<float, 16>(pp + 16 + 16 * k) * c; }); } }
      const vf dv = den;
      unroll<8>([&](auto ki) { constexpr int k = decltype(ki)::value; x[hd * 8 + k] = vdiv(o[k], dv) * block_load<float, 16>(sg + blk * 512 + (hd * 8 + k) * 16); }); });
    const float als = v_a8_tail(x, qx, alpha, MR, tok, blk, 8);
    v_scales((uint32_t)it.get_local_id(0), 8, tok, R, MR, als, us_out, Aw); });
}
// ---- histogram of the monotone float key of the scores of row j: level 0: key >> 22 (1024 bins); level 1: (key >> 11) & 2047 of the
// keys whose level-0 bin is thr[4 j].  One thread per 256 tokens, 10 threads share a histogram in SLM (atomic increments) and add its
// non-zero bins to hg[j][2048] (which the scan kernels read and clear).
inline void k_hist(sycl::queue& q, int J, int level, const float* s, const int* thr, uint32_t* hg) {
  q.parallel_for(sycl::nd_range<1>((size_t)J * (VOC / 256), 10), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<8192>();
    const uint32_t lid = (uint32_t)it.get_local_id(0); const size_t gid = it.get_global_id(0); const int j = (int)(gid / (VOC / 256)), sl = (int)(gid % (VOC / 256));
    for (uint32_t b = lid; b < 32; b += 10) slm_block_store<uint32_t, 64>(b * 256, simd<uint32_t, 64>(0u));
    barrier();
    const float* sr = s + (size_t)j * VOC + (size_t)sl * 256; const uint32_t t1 = level ? (uint32_t)thr[j * 4] : 0u;
    unroll<16>([&](auto vi_) { constexpr int v = decltype(vi_)::value; const vu u = bits(block_load<float, 16>(sr + 16 * v)); vu key = u | 0x80000000u; key.merge(~u, u >= 0x80000000u);
      if (level == 0) slm_atomic_update<atomic_op::inc, uint32_t, 16>((key >> 22) * 4u, simd_mask<16>(1));
      else slm_atomic_update<atomic_op::inc, uint32_t, 16>(((key >> 11) & 2047u) * 4u, (key >> 22) == t1); });
    barrier();
    for (uint32_t b = lid; b < 32; b += 10) { simd<uint32_t, 64> hv = slm_block_load<uint32_t, 64>(b * 256);
      unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; const vu x = hv.template select<16, 1>(16 * k);
        atomic_update<atomic_op::add, uint32_t, 16>(hg + (size_t)j * 2048, vu((b * 64 + 16 * k) * 4, 4), x, x != 0u); }); } });
}
// ---- head: projections of the 15 draft rows onto the 512 SHEAD directions and the 256 selector directions ----
inline void k_proj(sycl::queue& q, const float* hf, const float* bt, const float* hpw, float* proj) {
  q.parallel_for(sycl::range<1>(768), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int k = (int)it[0]; const float* w = k < 512 ? bt + (size_t)k * H : hpw + (size_t)(k - 512) * H;
    vf acc[15]; unroll<15>([&](auto ji) { acc[decltype(ji)::value] = 0.f; });
    for (int c = 0; c < 80; ++c) { simd<float, 64> wv = block_load<float, 64>(w + c * 64);
      unroll<15>([&](auto ji) { constexpr int j = decltype(ji)::value; simd<float, 64> xv = block_load<float, 64>(hf + (size_t)j * H + c * 64);
        unroll<4>([&](auto qi) { constexpr int kk = decltype(qi)::value; const vf a = wv.template select<16, 1>(16 * kk), b = xv.template select<16, 1>(16 * kk); acc[j] = acc[j] + a * b; }); }); }
    unroll<15>([&](auto ji) { constexpr int j = decltype(ji)::value; proj[j * 768 + k] = v_hsum(acc[j]); }); });
}
// 16-bit fixed point copy of the 512 projections of each row (hi: signed byte, lo: unsigned byte) in the XMX input layout
// [32-column chunk][row of 16][32 bytes]; rows >= J are zero.  qs[j] = value of one unit.
inline void k_qprep(sycl::queue& q, int J, const float* proj, int8_t* qh, uint8_t* ql, float* qs) {
  q.parallel_for(sycl::range<1>(16), [=](sycl::id<1> it) { const int j = (int)it[0]; float m = 0.f;
    if (j < J) for (int k = 0; k < 512; ++k) m = sycl::fmax(m, sycl::fabs(proj[j * 768 + k]));
    const float sc = (j < J && m > 0.f && m < 3.0e38f) ? 32767.f / m : 0.f; if (j < 15) qs[j] = sc > 0.f ? m / 32767.f : 0.f;
    for (int k = 0; k < 512; ++k) { const int v = j < J ? (int)sycl::rint(proj[j * 768 + k] * sc) : 0; const int idx = (k >> 5) * 512 + j * 32 + (k & 31);
      qh[idx] = (int8_t)(v >> 8); ql[idx] = (uint8_t)(v & 255); } });
}
// low-rank scores of the whole vocabulary for 15 rows: one thread per 16 tokens, XMX dot products (a8 x q16), then x as[v]
inline void k_score(sycl::queue& q, const int32_t* at, const float* as, const int8_t* qh, const uint8_t* ql, const float* qs, float* s) {
  q.parallel_for(sycl::range<1>(VOC / 16), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const size_t tg = it[0];
    simd<int, 128> ah[2], al[2]; unroll<2>([&](auto di) { constexpr int d = decltype(di)::value; ah[d] = 0; al[d] = 0; });
    unroll<16>([&](auto ci) { constexpr int cc = decltype(ci)::value; simd<int, 128> B; const int32_t* bp = at + (tg * 16 + cc) * 128;
      B.template select<64, 1>(0) = block_load<int, 64>(bp); B.template select<64, 1>(64) = block_load<int, 64>(bp + 64);
      unroll<2>([&](auto di) { constexpr int d = decltype(di)::value;
        const simd<int, 64> Ah = block_load<int, 64>((const int*)(qh + cc * 512 + d * 256)), Al = block_load<int, 64>((const int*)(ql + cc * 512 + d * 256));
        ah[d] = xmx::dpas<8, 8, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(ah[d], B, Ah);
        al[d] = xmx::dpas<8, 8, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::u8>(al[d], B, Al); }); });
    const vf av = block_load<float, 16>(as + tg * 16);
    unroll<15>([&](auto ji) { constexpr int j = decltype(ji)::value;
      const simd<int, 16> sh = ah[j / 8].template select<16, 1>((j % 8) * 16), sl = al[j / 8].template select<16, 1>((j % 8) * 16);
      const vf fh = sh, fl = sl; block_store<float, 16>(s + (size_t)j * VOC + tg * 16, ((fh * 256.f + fl) * qs[j]) * av); }); });
}
inline uint32_t skey(float f) { const uint32_t u = sycl::bit_cast<uint32_t>(f); return (u & 0x80000000u) ? ~u : (u | 0x80000000u); }
// exact re-scoring of the collected candidates with the int8 copy of lm_head
inline void k_rescore(sycl::queue& q, int J, int CAP, const int8_t* w8, const float* ws, const float* hf, const int* ci, const int* cnt, float* cv) {
  q.parallel_for(sycl::range<1>((size_t)J * CAP), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int j = (int)(it[0] / CAP), e = (int)(it[0] % CAP);
    if (e >= cnt[j]) { cv[it[0]] = NINF; return; }
    const int v = ci[it[0]]; const int8_t* w = w8 + (size_t)v * H; const float* x = hf + (size_t)j * H; vf acc = 0.f;
    for (int c = 0; c < 80; ++c) { simd<int8_t, 64> wb = block_load<int8_t, 64>(w + c * 64); simd<float, 64> xv = block_load<float, 64>(x + c * 64);
      unroll<4>([&](auto qi) { constexpr int k = decltype(qi)::value; const simd<int8_t, 16> b8 = wb.template select<16, 1>(16 * k); const simd<int, 16> wi = b8; const vf wf = wi, xf = xv.template select<16, 1>(16 * k);
        acc = acc + wf * xf; }); }
    cv[it[0]] = v_hsum(acc) * ws[v]; });
}
}  // namespace dx

struct Draft {
  static constexpr int NJ = 15, CAP = 1024, NC = VOC / 256, MASK = 248070;     // NC slices of 256 tokens (histograms, collect)
  bool on = false; Engine* E = nullptr; Container C;
  Tensor fc, L[5][9];        // per layer: q k v o gate up down akp mkp
  float *hnorm = nullptr, *fnorm = nullptr, *ln1[5], *ln2[5], *qn[5], *kn[5], *abase[5], *mbase[5], *hpw = nullptr, *sg = nullptr;
  uint16_t *rk = nullptr, *rv = nullptr;                 // rings [5 layers][8 heads][128 key groups][dx::KVG] bf16 (XMX layouts)
  const uint16_t *pred = nullptr, *succ = nullptr;       // host (mapped file): [V][256] bf16
  int8_t* sk_w = nullptr; int32_t* sk_at = nullptr; float *sk_bt = nullptr, *sk_as = nullptr, *sk_ws = nullptr; int sk_m = 512;
  uint16_t *bk, *bv; float *cap = nullptr, *h, *hmask, *x1, *dyn, *yq, *att, *proj, *sks, *cv, *alpha, *us2[2], *rope, *qs; int8_t *qx, *qh; uint8_t* ql; int32_t* Aw; int64_t* partw;
  int *ci, *cnt, *thr, *ids; uint32_t* h1;
  int ui = 0, ring_lo = 0, ring_hi = 0, max_ctx = 0, wforce = 0, anp = 0; int tokh = 0;
  std::vector<int> h_ci, h_cnt; std::vector<float> h_cv, h_hp;
  double ms_ctx = 0, ms_blk = 0, ms_head = 0, ms_sel = 0; long n_ctx = 0, n_blk = 0; bool prof = false;
  // YX_DPROF=2: wait after every launch and accumulate the time per kernel kind
  enum { TG, TNORM, TDYN, TQKV, TATT, TA8, TGLU, TEMB, TPROJ, TQP, TSCORE, THIST, TCOLL, TRESC, TCAP, TRING, TN }; double tacc[TN] = {}, tlast = 0; bool prof2 = false;
  void tk(int id) { if (!prof2) return; E->q.wait(); const double t = now(); tacc[id] += t - tlast; tlast = t; }
  void t0k() { if (prof2) { E->q.wait(); tlast = now(); } }
  void report(FILE* f) { static const char* nm[TN] = {"gemm", "norm", "dyn", "qkv", "attn", "a8att", "glu", "embed", "proj", "qprep", "score", "hist", "collect", "rescore", "a8cap", "ring"};
    fprintf(f, "[xe] drafter profile (cumulative, incl. waits): ctx %.3f ms x %ld, block %.3f ms + head %.3f ms + select %.3f ms x %ld\n", n_ctx ? ms_ctx / n_ctx : 0.0, n_ctx,
            n_blk ? ms_blk / n_blk : 0.0, n_blk ? ms_head / n_blk : 0.0, n_blk ? ms_sel / n_blk : 0.0, n_blk);
    if (prof2) { fprintf(f, "[xe] per kernel kind, ms per draft step:"); for (int i = 0; i < TN; ++i) fprintf(f, " %s %.3f", nm[i], tacc[i] / std::max(1L, n_blk)); fprintf(f, "\n"); } }

  void load(Engine& e, const std::string& path, int mctx) {
    if (on) return;
    E = &e; sycl::queue& q = e.q; max_ctx = mctx; prof = std::getenv("YX_DPROF") != nullptr; prof2 = prof && atoi(std::getenv("YX_DPROF")) >= 2; if (const char* w = std::getenv("YX_DW")) wforce = atoi(w); if (const char* w = std::getenv("YX_DNP")) anp = atoi(w);
    C.open(path);
    if (C.hdr["model"].value("arch", std::string()) != "bdraft2") throw std::runtime_error("load_bdraft: not a bdraft2 container: " + path);
    auto mk = [&](const std::string& name) { Tensor t = upload(q, C, name); if (t.bits != 8 || t.sg_out) throw std::runtime_error("load_bdraft: unsupported tensor " + name);
      t.part = e.dev<int64_t>((size_t)MRMAX * t.N); return t; };
    static const char* FN[9] = {"q", "k", "v", "o", "gate", "up", "down", "akp", "mkp"};
    fc = mk("bdraft2.fc"); sg = fc.sg_in;
    for (int i = 0; i < 5; ++i) { const std::string pf = "bdraft2.layers." + std::to_string(i) + ".";
      for (int k = 0; k < 9; ++k) L[i][k] = mk(pf + FN[k]);
      ln1[i] = upload_vec(q, C, pf + "ln1", H); ln2[i] = upload_vec(q, C, pf + "ln2", H); qn[i] = upload_vec(q, C, pf + "q_norm", 128); kn[i] = upload_vec(q, C, pf + "k_norm", 128);
      abase[i] = upload_vec(q, C, pf + "abase", 4 * H); mbase[i] = upload_vec(q, C, pf + "mbase", 4 * H); }
    hnorm = upload_vec(q, C, "bdraft2.hidden_norm", H); fnorm = upload_vec(q, C, "bdraft2.norm", H); hpw = upload_vec(q, C, "bdraft2.sel.hp", (size_t)256 * H);
    { const json& a = C.vector("bdraft2.sel.pred"); const json& b = C.vector("bdraft2.sel.succ");
      if (a["n"].get<size_t>() != (size_t)VOC * 256 || b["n"].get<size_t>() != (size_t)VOC * 256) throw std::runtime_error("load_bdraft: bad selector tables");
      pred = (const uint16_t*)(C.base + a["off"].get<uint64_t>()); succ = (const uint16_t*)(C.base + b["off"].get<uint64_t>()); }
    rk = e.dev<uint16_t>((size_t)5 * 8 * 2048 * 128); rv = e.dev<uint16_t>((size_t)5 * 8 * 2048 * 128);
    cap = e.dev<float>((size_t)5 * MRMAX * H); h = e.dev<float>((size_t)MRMAX * H); hmask = e.dev<float>((size_t)MRMAX * H); x1 = e.dev<float>((size_t)MRMAX * H);
    dyn = e.dev<float>(MRMAX * 1280); yq = e.dev<float>(MRMAX * 4096); att = e.dev<float>((size_t)8 * MRMAX * 32 * 144); bk = e.dev<uint16_t>(8 * dx::KVG); bv = e.dev<uint16_t>(8 * dx::KVG);
    proj = e.dev<float>(16 * 768); sks = e.dev<float>((size_t)NJ * VOC); cv = e.dev<float>(NJ * CAP); ci = e.dev<int>(NJ * CAP); cnt = e.dev<int>(16); thr = e.dev<int>(16 * 4); ids = e.dev<int>(16);
    alpha = e.dev<float>(MRMAX * 50); us2[0] = e.dev<float>(MRMAX); us2[1] = e.dev<float>(MRMAX); Aw = e.dev<int32_t>(MRMAX * 50); qx = e.dev<int8_t>((size_t)MRMAX * 25600);
    qh = e.dev<int8_t>(16 * 512); ql = e.dev<uint8_t>(16 * 512); qs = e.dev<float>(16); partw = e.dev<int64_t>((size_t)10 * MRMAX * 6144);
    h1 = e.dev<uint32_t>(NJ * 2048);
    h_ci.resize(NJ * CAP); h_cv.resize(NJ * CAP); h_cnt.resize(16); h_hp.resize(16 * 768);
    { std::vector<float> rt((size_t)(max_ctx + 16) * 128); float inv[64];
      for (int i = 0; i < 64; ++i) inv[i] = std::exp2(-(float)(2 * i) / 128.f * 23.253496664211536f);
      for (int p = 0; p < max_ctx + 16; ++p) for (int i = 0; i < 64; ++i) { const float a = (float)p * inv[i]; rt[(size_t)p * 128 + i] = std::cos(a); rt[(size_t)p * 128 + 64 + i] = std::sin(a); }
      rope = to_dev(q, rt.data(), rt.size()); }
    { int m = MASK; q.memcpy(ids, &m, 4).wait(); fx::k_embed(q, 1, e.emb_codes, e.emb_lut, e.emb_sc, e.emb_sg, hmask, ids);
      std::vector<float> row((size_t)MRMAX * H); q.memcpy(row.data(), hmask, H * 4).wait(); for (int r = 1; r < MRMAX; ++r) std::copy(row.begin(), row.begin() + H, row.begin() + (size_t)r * H);
      q.memcpy(hmask, row.data(), row.size() * 4).wait(); }
    load_shead(path);
    q.wait(); on = true;
  }
  void load_shead(const std::string& dpath) {
    sycl::queue& q = E->q; const char* s = std::getenv("YSKN_BDR_SHEAD"); const char* m = std::getenv("YSKN_BDR_SHEAD_M");
    std::string fn = (s && s[0] && std::string(s) != "0") ? std::string(s) : dpath.substr(0, dpath.find_last_of('.')) + ".shead";
    sk_m = (m && m[0]) ? atoi(m) : 512; if (sk_m < 16 || sk_m > CAP) throw std::runtime_error("load_bdraft: YSKN_BDR_SHEAD_M must be 16.." + std::to_string(CAP) + " in the Xe port");
    int fd = ::open(fn.c_str(), O_RDONLY); if (fd < 0) throw std::runtime_error("SHEAD: cannot open " + fn + " (the Xe port needs the SHEAD drafter head)");
    struct stat st; fstat(fd, &st); const size_t size = st.st_size; const uint8_t* b = (const uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0); ::close(fd);
    if (b == MAP_FAILED) throw std::runtime_error("SHEAD: mmap failed");
    struct Un { const uint8_t* p; size_t n; ~Un() { munmap((void*)p, n); } } un{b, size};
    const size_t V = VOC, R = 512, need = 24 + R * H * 4 + V * R + V * 4 + V * H + V * 4; int32_t hd[4];
    if (size < 24 || std::memcmp(b, "SHEAD001", 8)) throw std::runtime_error("SHEAD: bad header"); std::memcpy(hd, b + 8, 16);
    if (hd[0] != VOC || hd[1] != H || hd[2] != 512 || size != need) throw std::runtime_error("SHEAD: (V, H, r) or the size of the file do not match the engine");
    const uint8_t* p = b + 24;
    sk_bt = to_dev(q, (const float*)p, R * H); p += R * H * 4;
    { const int8_t* a = (const int8_t*)p; std::vector<int32_t> at(V * 128);       // XMX weight layout: [16 tokens][32-column chunk][dword k of 8][token lane]
      for (size_t tg = 0; tg < V / 16; ++tg) for (int cc = 0; cc < 16; ++cc) { int32_t* d = at.data() + (tg * 16 + cc) * 128;
        for (int ln = 0; ln < 16; ++ln) { const int8_t* r = a + (tg * 16 + ln) * R + cc * 32; for (int k = 0; k < 8; ++k) std::memcpy(&d[k * 16 + ln], r + 4 * k, 4); } }
      sk_at = to_dev(q, at.data(), at.size()); p += V * R; }
    sk_as = to_dev(q, (const float*)p, V); p += V * 4;
    sk_w = to_dev(q, (const int8_t*)p, V * H); p += V * H;
    sk_ws = to_dev(q, (const float*)p, V);
  }
  void reset() { ring_lo = ring_hi = 0; }
  // ---- GEMM over the drafter's scratch (qx / Aw), all matrices of the group in one launch, split over W column workers ----
  void gemm(std::initializer_list<Tensor*> ts, int MR) {
    sycl::queue& q = E->q; GemmT gt[3]; int nt = 0, thr_ = 0; for (Tensor* T : ts) { gt[nt++] = GemmT{T->stream, T->part, T->N}; thr_ += T->N / 16; }
    Tensor* T0 = *ts.begin(); int W = std::max(1, std::min(T0->G, (640 + thr_ / 2) / thr_)); if (wforce) W = std::min(wforce, W);
    while ((thr_ * W) % GEMM_LS) ++W;                              // the launch needs whole work-groups
    int64_t* out[3] = {gt[0].part, gt[1].part, gt[2].part}; size_t n[3] = {0, 0, 0}, off[3] = {0, 0, 0};
    if (W > 1) { size_t o = 0; for (int i = 0; i < nt; ++i) { n[i] = (size_t)MR * gt[i].N; off[i] = o; gt[i].part = partw + o; o += (size_t)W * n[i]; } }
    if (MR == 8) gemm_multi<8, 8>(q, gt, nt, T0->lut, qx, Aw, T0->Kc, W); else gemm_multi<8, 16>(q, gt, nt, T0->lut, qx, Aw, T0->Kc, W);
    if (W > 1) { const int64_t* pw = partw; int64_t *o0 = out[0], *o1 = out[1], *o2 = out[2]; const size_t n0 = n[0], n1 = n[1], n2 = n[2], f1 = off[1], f2 = off[2];
      q.parallel_for(sycl::range<1>(n0 + n1 + n2), [=](sycl::id<1> id) { size_t x = id[0]; const int64_t* in; int64_t* o; size_t nn;
        if (x < n0) { in = pw + x; o = o0 + x; nn = n0; } else if (x < n0 + n1) { x -= n0; in = pw + f1 + x; o = o1 + x; nn = n1; } else { x -= n0 + n1; in = pw + f2 + x; o = o2 + x; nn = n2; }
        int64_t s = 0; for (int w = 0; w < W; ++w) s += in[(size_t)w * nn]; *o = s; }); }
    tk(TG);
  }
  float* us_r() const { return us2[ui]; }
  float* us_w() { ui ^= 1; return us2[ui]; }
  // ---- context rows: captured hidden states of rows 0..n-1 (positions pos0..) -> ring entries of the 5 layers ----
  void ctx(int n, int pos0) {
    if (!on || n <= 0) return; if (n > MRMAX || pos0 < 0 || pos0 + n > max_ctx) throw std::runtime_error("bdraft: bad context rows");
    sycl::queue& q = E->q; const int MR = n <= 8 ? 8 : 16; const double t0 = prof ? now() : 0; t0k();
    dx::k_a8cap(q, n, MR, cap, sg, qx, alpha); dx::k_prep(q, n, MR, 50, alpha, us_w(), Aw); tk(TCAP);
    gemm({&fc}, MR);
    { const float* ui_ = us_r(); dx::k_norm(q, n, MR, dx::NormA{2, nullptr, fc.part, fc.tau, nullptr, nullptr, ui_, hnorm, sg, nullptr, qx, alpha, us_w(), Aw, 1e-6f}); tk(TNORM); }
    gemm({&L[0][1], &L[0][2], &L[1][1]}, MR); gemm({&L[1][2], &L[2][1], &L[2][2]}, MR); gemm({&L[3][1], &L[3][2], &L[4][1]}, MR); gemm({&L[4][2]}, MR);
    dx::RingA P; for (int i = 0; i < 5; ++i) { P.ak[i] = L[i][1].part; P.av[i] = L[i][2].part; P.tk[i] = L[i][1].tau; P.tv[i] = L[i][2].tau; P.kn[i] = kn[i]; }
    P.us = us_r(); P.rope = rope; P.rk = rk; P.rv = rv; P.eps = 1e-6f; P.p = pos0;
    dx::k_ring(q, n, P); tk(TRING);
    if (pos0 > ring_hi || pos0 < ring_lo) ring_lo = pos0;
    ring_hi = pos0 + n;
    if (prof) { q.wait(); ms_ctx += now() - t0; ++n_ctx; }
  }
  // ---- one-shot draft block of R rows (row 0 = frontier token `tok` at position p, rows 1.. = mask token) up to the re-scored candidates ----
  void block(int R, int tok, int p) {
    sycl::queue& q = E->q; const int MR = R <= 8 ? 8 : 16, J = R - 1; const float eps = 1e-6f; const double t0 = prof ? now() : 0; t0k();
    const int lo = ring_hi >= p ? ring_lo : p;                 // ring entries [lo, p) are valid context
    const int NP = std::max(1, std::min(8, anp ? anp : 640 / (R * 16)));    // attention parts per (row, head pair): fill the ~640 hardware threads
    tokh = tok; q.memcpy(ids, &tokh, 4).wait(); q.memcpy(h, hmask, (size_t)R * H * 4);
    fx::k_embed(q, 1, E->emb_codes, E->emb_lut, E->emb_sc, E->emb_sg, h, ids); tk(TEMB);
    for (int i = 0; i < 5; ++i) { Tensor* T = L[i];
      { const float* u = us_r(); const Tensor* D = i ? &L[i - 1][6] : nullptr;
        dx::k_norm(q, R, MR, dx::NormA{i ? 1 : 0, h, D ? D->part : nullptr, D ? D->tau : nullptr, dyn, i ? mbase[i - 1] + 2 * H : nullptr, u, ln1[i], sg, x1, qx, alpha, us_w(), Aw, eps}); tk(TNORM); }
      gemm({&T[7]}, MR);
      { const float* u = us_r(); dx::k_dyn(q, R, MR, dx::DynA{T[7].part, T[7].tau, u, x1, abase[i], sg, dyn, qx, alpha, us_w(), Aw}); tk(TDYN); }
      gemm({&T[0], &T[1], &T[2]}, MR);
      dx::k_qkv(q, R, dx::QkvA{T[0].part, T[1].part, T[2].part, T[0].tau, T[1].tau, T[2].tau, us_r(), qn[i], kn[i], rope, yq, bk, bv, eps, p}); tk(TQKV);
      dx::k_attn(q, dx::AttA{yq, bk, bv, rk + (size_t)i * 8 * 2048 * 128, rv + (size_t)i * 8 * 2048 * 128, att, p, lo, R, NP}); tk(TATT);
      dx::k_a8att(q, R, MR, NP, att, sg, qx, alpha, us_w(), Aw); tk(TA8);
      gemm({&T[3]}, MR);
      { const float* u = us_r(); dx::k_norm(q, R, MR, dx::NormA{1, h, T[3].part, T[3].tau, dyn, abase[i] + 2 * H, u, ln2[i], sg, x1, qx, alpha, us_w(), Aw, eps}); tk(TNORM); }
      gemm({&T[8]}, MR);
      { const float* u = us_r(); dx::k_dyn(q, R, MR, dx::DynA{T[8].part, T[8].tau, u, x1, mbase[i], sg, dyn, qx, alpha, us_w(), Aw}); tk(TDYN); }
      gemm({&T[4], &T[5]}, MR);
      { const float* u = us_r(); dx::k_glu(q, R, MR, dx::GluA{T[4].part, T[5].part, T[4].tau, T[5].tau, u, sg, qx, alpha, us_w(), Aw}); tk(TGLU); }
      gemm({&T[6]}, MR); }
    { const float* u = us_r(); dx::k_norm(q, R, MR, dx::NormA{1, h, L[4][6].part, L[4][6].tau, dyn, mbase[4] + 2 * H, u, fnorm, sg, x1, qx, alpha, us_w(), Aw, eps}); tk(TNORM); }
    if (prof) { q.wait(); ms_blk += now() - t0; ++n_blk; }
    const double t1 = prof ? now() : 0;
    // SHEAD: projections -> low-rank scores of the vocabulary -> the sk_m best per row (threshold from a 2-level histogram of the
    // monotone float key, 21 bits; ties at the threshold are all taken if they fit) -> exact re-scoring
    const float* hf = x1 + H; const int M = sk_m; const float* s = sks; uint32_t* hp = h1; int *tp = thr, *cp = cnt, *cip = ci;
    dx::k_proj(q, hf, sk_bt, hpw, proj); tk(TPROJ); dx::k_qprep(q, J, proj, qh, ql, qs); tk(TQP); dx::k_score(q, sk_at, sk_as, qh, ql, qs, sks); tk(TSCORE);
    dx::k_hist(q, J, 0, sks, thr, h1);
    q.parallel_for(sycl::range<1>(J), [=](sycl::id<1> it) { const int j = (int)it[0]; uint32_t acc = 0; int t1 = 0, ab = 0; bool f = false;
      for (int b = 1023; b >= 0; --b) { const uint32_t c = hp[j * 2048 + b]; hp[j * 2048 + b] = 0; if (!f && acc + c >= (uint32_t)M) { t1 = b; ab = (int)acc; f = true; } acc += c; }
      tp[j * 4] = t1; tp[j * 4 + 1] = ab; });
    dx::k_hist(q, J, 1, sks, thr, h1);
    q.parallel_for(sycl::range<1>(J), [=](sycl::id<1> it) { const int j = (int)it[0]; uint32_t acc = (uint32_t)tp[j * 4 + 1]; int b2 = 0, ab = (int)acc, neq = 0; bool f = false;
      for (int b = 2047; b >= 0; --b) { const uint32_t c = hp[j * 2048 + b]; hp[j * 2048 + b] = 0; if (!f && acc + c >= (uint32_t)M) { b2 = b; ab = (int)acc; neq = (int)c; f = true; } acc += c; }
      const int take = ab + neq <= CAP; tp[j * 4 + 2] = (int)(((uint32_t)tp[j * 4] << 11) | (uint32_t)b2); tp[j * 4 + 3] = take; cp[j] = 0; });
    tk(THIST);
    q.parallel_for(sycl::range<1>((size_t)J * NC), [=](sycl::id<1> it) { const int j = (int)it[0] / NC, t = (int)it[0] % NC; const float* sr = s + (size_t)j * VOC + (size_t)t * 256; const uint32_t p21 = (uint32_t)tp[j * 4 + 2]; const bool take = tp[j * 4 + 3] != 0;
      for (int v = 0; v < 256; ++v) { const uint32_t k = dx::skey(sr[v]) >> 11; if (k > p21 || (k == p21 && take)) {
        sycl::atomic_ref<int, sycl::memory_order::relaxed, sycl::memory_scope::device, sycl::access::address_space::global_space> c(cp[j]);
        const int e = c.fetch_add(1); if (e < CAP) cip[j * CAP + e] = t * 256 + v; } } });
    tk(TCOLL);
    dx::k_rescore(q, J, CAP, sk_w, sk_ws, hf, ci, cnt, cv); tk(TRESC);
    if (prof) { q.wait(); ms_head += now() - t1; }
  }
  static double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  static float bf(uint16_t u) { uint32_t x = (uint32_t)u << 16; float f; std::memcpy(&f, &x, 4); return f; }
  // ---- top-16 of the candidates and the chain selection with the rank-256 "previous token -> next token" correction (host) ----
  // out[0..J): the drafted tokens for positions p + 1 ..; sampling (Gumbel-max, version 3) uses the main sampler's noise, cpl = 850
  void select(int J, int tok, int p, const SmpParams& smp, int* out) {
    sycl::queue& q = E->q; const double t0 = prof ? now() : 0;
    q.memcpy(h_cnt.data(), cnt, 16 * 4); q.memcpy(h_ci.data(), ci, (size_t)J * CAP * 4); q.memcpy(h_cv.data(), cv, (size_t)J * CAP * 4); q.memcpy(h_hp.data(), proj, 16 * 768 * 4).wait();
    const int cpl = (smp.on && smp.version == 3) ? 850 : -1; int prev = tok; std::vector<std::pair<float, int>> c; c.reserve(CAP);
    auto better = [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); };
    for (int j = 0; j < J; ++j) {
      const int n = std::min(h_cnt[j], CAP); c.clear(); for (int e = 0; e < n; ++e) c.emplace_back(h_cv[(size_t)j * CAP + e], h_ci[(size_t)j * CAP + e]);
      const int nc = std::min(16, n); if (!nc) { out[j] = prev = 0; continue; }
      std::partial_sort(c.begin(), c.begin() + nc, c.end(), better);
      const float* hpj = h_hp.data() + j * 768 + 512; const uint16_t* pr = pred + (size_t)prev * 256; float a[256], score[16];
      for (int r = 0; r < 256; ++r) a[r] = bf(pr[r]) * hpj[r];
      for (int k = 0; k < nc; ++k) { const uint16_t* sr = succ + (size_t)c[k].second * 256; float sm = 0.f; for (int r = 0; r < 256; ++r) sm += a[r] * bf(sr[r]); score[k] = c[k].first + sm; }
      int b = 0; for (int k = 1; k < nc; ++k) if (score[k] > score[b]) b = k;
      if (cpl > 0) { const double td = 1e-3 * cpl; int ord[16];
        for (int k = 0; k < nc; ++k) { int rnk = 0; for (int i = 0; i < nc; ++i) rnk += (score[i] > score[k] || (score[i] == score[k] && c[i].second < c[k].second)) ? 1 : 0; ord[rnk] = k; }
        double z[16], ev[16], tot = 0, best = -INFINITY;
        for (int k = 0; k < nc; ++k) { z[k] = ((double)score[k] - (double)score[ord[0]]) / td; ev[k] = std::exp(z[k]); }
        double pre[16]; for (int i = 0; i < nc; ++i) { pre[i] = tot; tot += ev[ord[i]]; }
        for (int i = 0; i < nc; ++i) { const int k = ord[i]; if (!(pre[i] < 0.95 * tot)) continue;
          const uint64_t hsh = Head::mix(Head::mix(Head::mix(smp.seed) ^ (uint64_t)((int64_t)p + 1 + j)) ^ ((uint64_t)(uint32_t)c[k].second * 0xD6E8FEB86659FD93ull));
          const double u = ((double)(hsh >> 11) + 0.5) * 1.1102230246251565e-16, v = z[k] - std::log(-std::log(u)); if (v > best) { best = v; b = k; } } }
      out[j] = prev = c[b].second;
    }
    if (prof) ms_sel += now() - t0;
  }
  // bdraft_step of the reference: feed the rows committed by the previous pass, then draft R - 1 tokens (R = 0: context only)
  void step(int n_ctx, int pos_ctx, int R, int tok, int p, const SmpParams& smp, int* out) {
    if (n_ctx) ctx(n_ctx, pos_ctx);
    if (R) { block(R, tok, p); select(R - 1, tok, p, smp, out); }
  }
};
}  // namespace yx
