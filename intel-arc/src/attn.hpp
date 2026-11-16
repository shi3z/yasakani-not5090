// SPDX-License-Identifier: Apache-2.0
// Full-attention layer with its KV5 cache for Intel Xe2 in SYCL.  Math: spec/C_attention.md (+ Errata), executable
// reference: ref/core.py class Attn (bit-exact target).  Port notes: spec/C_port_notes.md.
//   attn_forward(E, A, M, pos0):  q/k/v GEMMs -> post (norm, RoPE, rotation, gate) -> cache write (INT8 row, trellis
//   encode, pack / store8) -> attention (splits, 32-slot tiles, own key) -> combine, inverse rotation, gate, a8 -> o GEMM.
// No double precision on the device: the RoPE table is made on the host, exp2f(x) = pm_expf(x * LN2_F).
// Two implementations with identical results: ESIMD kernels (namespace ak, used when E.fast) and the scalar kernels
// inside attn_forward (YX_SCALAR=1).  tests/attn_regress.sh compares both with the reference bit for bit.
#pragma once
#include "engine.hpp"
#include <chrono>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
namespace yx {
constexpr int A_NH = 24, A_KVH = 4, A_HD = 256, A_GQ = 6, A_SPL = 42, A_SHORT = 31, A_LONG = 4096, A_NM = 9;
inline const int A_PSH = std::getenv("YX_PSH") ? atoi(std::getenv("YX_PSH")) : 9; inline const int A_PS = 1 << A_PSH;                   // mirror path: keys per part (absolute position / A_PS), fixed so that a row's result does not depend on the pass
constexpr int A_NQ = 2 * A_NH * A_HD, A_NKV = A_KVH * A_HD, A_NO = A_NH * A_HD;      // q_proj rows, k/v_proj rows, o_proj columns
constexpr float A_LOG2E = 1.4426950408889634f, A_LN2 = 0.6931471805599453f;

inline int g_attn_mode = [] { const char* e = std::getenv("YX_ATTN_FA"); return e ? atoi(e) : 0; }();      // attention kernels: 0 default; > 0 experiments, see attn_forward (tests may change it between passes)
inline double* attn_prof = nullptr;      // tests: when set, attn_forward waits after every stage and stores 7 time stamps (seconds)
// ---- device helpers ----
inline float pm_exp2f(float x) { return pm_expf(x * A_LN2); }                    // exp2f(0) == 1, exp2f(-inf) == 0 exactly
inline float s256(const float* v) { float t = 0.f; for (int w = 0; w < 8; ++w) t = t + xor_tree32(v + 32 * w); return t; }
inline float tree16(const float* a) {                                            // for o in (8,4,2,1): a[i] += a[i^o]; -> a[0]
  float b[8]; for (int i = 0; i < 8; ++i) b[i] = a[i] + a[i + 8];
  const float c0 = b[0] + b[4], c1 = b[1] + b[5], c2 = b[2] + b[6], c3 = b[3] + b[7];
  return (c0 + c2) + (c1 + c3);
}
inline void kv_rot(float* b, const float* sg, bool inverse) {                    // kvx_rotate, 256 values in place
  if (!inverse) for (int d = 0; d < 256; ++d) b[d] = b[d] * sg[d];
  for (int h = 1; h < 256; h <<= 1)
    for (int p = 0; p < 256; p += 2 * h)
      for (int i = p; i < p + h; ++i) { const float a = b[i], c = b[i + h]; b[i] = a + c; b[i + h] = a - c; }
  if (inverse) for (int d = 0; d < 256; ++d) b[d] = (b[d] * 0.0625f) * sg[d];
  else for (int d = 0; d < 256; ++d) b[d] = b[d] * 0.0625f;
}
inline float kv_store8f(const float* x, float* qf) {                             // kvx_store: integer-valued floats + scale
  float mx = 0.f; for (int d = 0; d < 256; ++d) mx = sycl::fmax(mx, sycl::fabs(x[d]));
  const float sc = fdiv(mx, 127.f);
  for (int d = 0; d < 256; ++d) { float v = 0.f; if (sc > 0.f) { v = sycl::rint(fdiv(x[d], sc)); v = v < -127.f ? -127.f : (v > 127.f ? 127.f : v); } qf[d] = v; }
  return sc;
}
inline void norm_rope(float* x, const float* w, const float* cs, float eps) {    // RMS norm with 1+w, RoPE on dims 0..63; cs = cos[32], sin[32]
  float t[256]; for (int d = 0; d < 256; ++d) t[d] = x[d] * x[d];
  const float r = pm_rsqrt(fdiv(s256(t), 256.f) + eps);
  for (int d = 0; d < 256; ++d) x[d] = rbf((x[d] * r) * (1.f + w[d]));
  for (int d = 0; d < 32; ++d) { const float a = x[d], b = x[d + 32], c = cs[d], s = cs[32 + d];
    x[d] = rbf(rbf(a * c) + rbf((-b) * s)); x[d + 32] = rbf(rbf(b * c) + rbf(a * s)); }
}
// 5-bit field e (0..63) of one chain (10 words, MSB-first bit stream)
inline uint32_t tb_field(const uint32_t* w, int e) {
  const int a = 5 * e, wi = a >> 5, off = a & 31; uint32_t v = w[wi] << off; if (off > 27) v |= w[wi + 1] >> (32 - off); return v >> 27;
}


// ---- host: KV5 profile (kvq_u5.bin, spec C 3.7) ----
struct KvGroup { int mode = 0, kk = 0, guard = 0, m = 0, has_mu = 0; float gain = 0.f; std::vector<float> mu, U[2]; std::vector<int> ci; };
struct KvProfile {
  std::vector<uint8_t> B; int nm = 0; float mult[A_NM] = {};
  uint16_t u16(size_t o) const { uint16_t v; std::memcpy(&v, &B[o], 2); return v; }
  uint32_t u32(size_t o) const { uint32_t v; std::memcpy(&v, &B[o], 4); return v; }
  void load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate); if (!f) throw std::runtime_error("cannot open " + path);
    B.resize((size_t)f.tellg()); f.seekg(0); f.read((char*)B.data(), B.size());
    if (B.size() < 96 || std::memcmp(B.data(), "KVQP", 4)) throw std::runtime_error("not a KVQP profile: " + path);
    nm = u16(24); if (nm != A_NM || u16(16) != 256) throw std::runtime_error("unsupported KVQP profile");
    std::memcpy(mult, &B[u32(32)], 4 * A_NM);
  }
  KvGroup group(int g) const {
    KvGroup P; const size_t go = u32(u32(40) + 4 * (size_t)g);
    P.mode = B[go]; P.kk = B[go + 1]; const int tix = B[go + 2]; P.guard = B[go + 3]; P.m = B[go + 4]; P.has_mu = B[go + 5];
    if (P.m > 2) throw std::runtime_error("KVQP: more than 2 low-rank terms");
    size_t o = go + 8;
    if (P.has_mu) { P.mu.resize(256); for (int d = 0; d < 256; ++d) P.mu[d] = h2f_host(u16(o + 2 * d)); o += 512; }
    for (int j = 0; j < P.m; ++j) { P.U[j].resize(256); for (int d = 0; d < 256; ++d) P.U[j][d] = h2f_host(u16(o + 2 * d)); o += 512; }
    const size_t to = u32(u32(36) + 4 * (size_t)tix); std::memcpy(&P.gain, &B[to + 4], 4);
    P.ci.resize(256); for (int s = 0; s < 256; ++s) P.ci[s] = (int16_t)u16(to + 12 + 2 * s);
    return P;
  }
};
// ---- host: RoPE table (pm_sincos_f of pm_math.cuh: double kernels, rounded to f32, then BF16) ----
inline double a_pmd(uint32_t hi, uint32_t lo) { const uint64_t u = ((uint64_t)hi << 32) | lo; double d; std::memcpy(&d, &u, 8); return d; }
inline void pm_sincos_host(float a, float& cs, float& sn) {
  const double x = (double)a;
  const double n = std::trunc(x * a_pmd(0x3FE45F30, 0x6DC9C883) + 0.5); const int64_t ni = (int64_t)n;
  const double r = ((x - n * a_pmd(0x3FF921FB, 0x54400000)) - n * a_pmd(0x3DD0B461, 0x1A600000)) - n * a_pmd(0x3BA3198A, 0x2E037073);
  const double z = r * r, v = z * r;
  const double R = a_pmd(0x3F811111, 0x1110F8A6) + z * (a_pmd(0xBF2A01A0, 0x19C161D5) + z * (a_pmd(0x3EC71DE3, 0x57B1FE7D) + z * (a_pmd(0xBE5AE5E6, 0x8A2B9CEB) + z * a_pmd(0x3DE5D93A, 0x5ACFD57C))));
  const double ks = r + v * (a_pmd(0xBFC55555, 0x55555549) + z * R);
  const double Rc = z * (a_pmd(0x3FA55555, 0x5555554C) + z * (a_pmd(0xBF56C16C, 0x16C15177) + z * (a_pmd(0x3EFA01A0, 0x19CB1590) + z * (a_pmd(0xBE927E4F, 0x809C52AD) + z * (a_pmd(0x3E21EE9E, 0xBDB4B1C4) + z * a_pmd(0xBDA8FAE9, 0xBE8838D4))))));
  uint64_t ur; std::memcpy(&ur, &r, 8); const uint32_t ix = (uint32_t)(ur >> 32) & 0x7fffffffu; const double zr = z * Rc;
  const double qx = ix > 0x3fe90000u ? 0.28125 : a_pmd((ix > 0x00200000u ? ix : 0x00200000u) - 0x00200000u, 0);
  const double kc = ix < 0x3FD33333u ? 1.0 - (0.5 * z - zr) : (1.0 - qx) - ((0.5 * z - qx) - zr);
  const int q = (int)(ni & 3);
  const double c = q == 0 ? kc : q == 1 ? -ks : q == 2 ? -kc : ks, s = q == 0 ? ks : q == 1 ? kc : q == 2 ? -ks : -kc;
  cs = (float)c; sn = (float)s;
}
inline float rbf_host(float f) { uint32_t u; std::memcpy(&u, &f, 4); u += 0x7fffu + ((u >> 16) & 1u); u &= 0xffff0000u; std::memcpy(&f, &u, 4); return f; }
inline std::vector<float> rope_table_host(int n) {                               // [n][64]: cos[32], sin[32], BF16 values
  static const uint32_t IF[32] = {
    0x3f800000, 0x3f1ab32b, 0x3ebaf81b, 0x3e61f835, 0x3e088d77, 0x3da50956, 0x3d47763f, 0x3cf11177, 0x3c91ad39, 0x3c301052, 0x3bd4ca15,
    0x3b80967d, 0x3b1b690d, 0x3abbd3ed, 0x3a6301e2, 0x3a092e02, 0x39a5cb60, 0x394860c1, 0x38f22ce2, 0x3892587e, 0x3830df52, 0x37d5c441,
    0x37812dab, 0x371c1fc4, 0x36bcb0c1, 0x36640cc6, 0x3609cf4a, 0x35a68e4c, 0x35494c57, 0x34f3499d, 0x3493048e, 0x3431af44};
  std::vector<float> t((size_t)n * 64);
  for (int p = 0; p < n; ++p) for (int i = 0; i < 32; ++i) { float fr; std::memcpy(&fr, &IF[i], 4); volatile float f = (float)p * fr; float c, s; pm_sincos_host(f, c, s);
    t[(size_t)p * 64 + i] = rbf_host(c); t[(size_t)p * 64 + 32 + i] = rbf_host(s); }
  return t;
}
inline float kv_sign_host(uint32_t d) { uint32_t x = d + 1; x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return (x & 1) ? -1.f : 1.f; }

// ---- data shared by all attention layers: tables and per-pass scratch ----
struct AttnShared {
  int max_ctx = 0; KvProfile prof;
  float *rope = nullptr, *sg = nullptr, *mult = nullptr;                         // [max_ctx][64], [256], [9]
  float *q16 = nullptr, *gate = nullptr, *y = nullptr, *y2 = nullptr;            // [MRMAX][24][256]  (y, y2: for tests)
  float *enc_r = nullptr, *enc_c = nullptr, *enc_e = nullptr; uint8_t* enc_st = nullptr;   // encoder: residual [MR][8][256], (c0, c1, rms, guard flag) [MR][8][4],
                                                                                 // tree32 errors [MR][8][9][8], states [MR][8][9][256]
  float *pm = nullptr, *pl = nullptr, *pacc = nullptr;                           // partials [MR][42][24], [MR][42][24][256]
  int* vxi = nullptr; uint8_t* vbp = nullptr;                                    // ESIMD trellis scratch per (row, group, scale, chain): xi [64], back pointers [64][16]
  // mirror path (decoded bf16 copies of K and V + XMX products): G key groups of 16, NP parts of A_PS keys
  bool mir = false; int G = 0, NP = 0, NQG = 12;                                  // NQG: query groups of 8 per KV head (6 * rows / 8)
  float* sc = nullptr;                                                           // scores [4 kvh][12 query groups][G][8 queries][16 keys]
  uint32_t *pa = nullptr, *qa = nullptr;                                         // weights (bf16 pairs) [4][12][G][8 queries][8 key pairs]; queries [4][12][16 dim chunks][8 queries][8 dim pairs]
  float *pm2 = nullptr, *pl2 = nullptr, *pacc2 = nullptr;                        // partials [MR][NP][24], [MR][NP][24][256]
  AttnShared(Engine& E, int max_ctx_, const std::string& profile) : max_ctx(max_ctx_) {
    prof.load(profile);
    { auto t = rope_table_host(max_ctx); rope = to_dev(E.q, t.data(), t.size()); }
    { float s[256]; for (int d = 0; d < 256; ++d) s[d] = kv_sign_host(d); sg = to_dev(E.q, s, 256); }
    mult = to_dev(E.q, prof.mult, A_NM);
    const int MRC = E.cap; NQG = (MRC * 6 + 7) / 8;
    const size_t R = (size_t)MRC * A_NO;
    q16 = E.dev<float>(R); gate = E.dev<float>(R); y = E.dev<float>(R); y2 = E.dev<float>(R);
    enc_r = E.dev<float>((size_t)MRC * 8 * 256); enc_c = E.dev<float>(MRC * 8 * 4); enc_e = E.dev<float>(MRC * 8 * A_NM * 8);
    enc_st = E.dev<uint8_t>((size_t)MRC * 8 * A_NM * 256);
    vxi = E.dev<int>((size_t)MRC * 8 * A_NM * 4 * 64); vbp = E.dev<uint8_t>((size_t)MRC * 8 * A_NM * 4 * 1024);
    G = (max_ctx + 15) / 16; NP = (max_ctx + A_PS - 1) / A_PS;
    { const char* o = std::getenv("YX_ATTN_OLD"); const char* gb = std::getenv("YX_MIRROR_GB"); const double lim = gb ? atof(gb) : 10.0;
      const double need = 16.0 * 2 * 4 * (double)G * 8192 + 4.0 * NQG * G * (512 + 256);                  // 16 layers x (K + V) mirrors + scores + weights
      mir = E.fast && !(o && atoi(o)) && need <= lim * 1073741824.0; }
    if (!mir) { pm = E.dev<float>(MRC * A_SPL * A_NH); pl = E.dev<float>(MRC * A_SPL * A_NH); pacc = E.dev<float>((size_t)MRC * A_SPL * A_NH * 256); }      // partials of the old path only
    if (mir) { sc = E.dev<float>((size_t)4 * NQG * G * 128); pa = E.dev<uint32_t>((size_t)4 * NQG * G * 64); qa = E.dev<uint32_t>((size_t)4 * NQG * 16 * 64);
      pm2 = E.dev<float>((size_t)MRC * NP * A_NH); pl2 = E.dev<float>((size_t)MRC * NP * A_NH); pacc2 = E.dev<float>((size_t)MRC * NP * A_NH * 256); }
  }
};
// KV profile: $YX_KV_PROFILE, else $YSKN_TB_PROFILE (set by the Python package), else kvq_u5.bin in the folder of the model file
inline std::string attn_default_profile(const Engine& E) { const char* e = std::getenv("YX_KV_PROFILE"); if (!e) e = std::getenv("YSKN_TB_PROFILE"); if (e) return e;
  const size_t k = E.model_path.find_last_of('/'); return (k == std::string::npos ? std::string("") : E.model_path.substr(0, k + 1)) + "kvq_u5.bin"; }
// one AttnShared per (engine, max_ctx), shared by the 16 layers
inline std::shared_ptr<AttnShared> attn_shared(Engine& E, int max_ctx, std::string profile = "") {
  if (profile.empty()) profile = attn_default_profile(E);
  static std::map<std::pair<Engine*, int>, std::weak_ptr<AttnShared>> reg; auto& w = reg[{&E, max_ctx}]; auto p = w.lock();
  if (!p) { p = std::make_shared<AttnShared>(E, max_ctx, profile); w = p; }
  return p;
}

// ---- one full-attention layer ----
struct Attn {
  int layer = 0, ord = 0, max_ctx = 0; bool vpk = true;
  Tensor q, k, v, o; float *qn = nullptr, *kn = nullptr;
  float* gf = nullptr;       // [8][5][256] per group (0..3 K heads, 4..7 V heads): encoder codebook, decode table (BF16), mean, U0, U1
  int* gci = nullptr;        // [8][256] integer codebook
  int* gmeta = nullptr;      // [8][4]: guard, m, has_mu, 0
  uint32_t *kp = nullptr, *vp = nullptr; float *kps = nullptr, *vps = nullptr;   // packed K (always), packed V (vpk): [4][max_ctx][40] words, [4][max_ctx]
  int8_t* v8 = nullptr; float* vs = nullptr;                                     // INT8 V (!vpk): [4][max_ctx][256], [4][max_ctx]
  uint16_t *mk = nullptr, *mv = nullptr;                                         // mirrors (sh->mir), bf16: K [4][G][16 dim chunks][8 dim pairs][16 keys][2], V [4][G][16 dim chunks][8 key pairs][16 dims][2]
  std::shared_ptr<AttnShared> sh;
  Attn() = default;
  Attn(Engine& E, int layer_, int max_ctx_) : Attn(E, layer_, attn_shared(E, max_ctx_)) {}
  Attn(Engine& E, int layer_, std::shared_ptr<AttnShared> sh_) : layer(layer_), ord(layer_ / 4), max_ctx(sh_->max_ctx), sh(sh_) {
    const std::string p = "layers." + std::to_string(layer) + ".self_attn."; sycl::queue& Q = E.q;
    q = upload(Q, E.C, p + "q_proj"); k = upload(Q, E.C, p + "k_proj"); v = upload(Q, E.C, p + "v_proj"); o = upload(Q, E.C, p + "o_proj");
    if (q.N != A_NQ || k.N != A_NKV || v.N != A_NKV || o.N != H || q.Kc != H || k.Kc != H || v.Kc != H || o.Kc != A_NO) throw std::runtime_error("unexpected attention tensor shapes");
    q.part = E.part; k.part = E.part + (size_t)E.cap * A_NQ; v.part = k.part + (size_t)E.cap * A_NKV; o.part = E.part;      // q/k/v are consumed before o is launched
    qn = upload_vec(Q, E.C, p + "q_norm", 256); kn = upload_vec(Q, E.C, p + "k_norm", 256);
    std::vector<float> f(8 * 5 * 256, 0.f); std::vector<int> ci(8 * 256), meta(8 * 4, 0); bool kpk = true;
    for (int g8 = 0; g8 < 8; ++g8) { const int kind = g8 >> 2, kvh = g8 & 3; const KvGroup P = sh->prof.group((ord * 4 + kvh) * 2 + kind);
      if (P.mode != 0 || P.kk != 5) throw std::runtime_error("KV profile group is not a 5-bit trellis group");
      const bool pk = P.guard == 0 && P.m == 0; if (kind) vpk = vpk && pk; else kpk = kpk && pk;
      float* F = &f[(size_t)g8 * 5 * 256];
      for (int s = 0; s < 256; ++s) { volatile float a = (float)P.ci[s] * 0.00390625f; volatile float c = a * P.gain; F[s] = c; F[256 + s] = rbf_host(c); ci[g8 * 256 + s] = P.ci[s]; }
      if (P.has_mu) for (int d = 0; d < 256; ++d) F[512 + d] = P.mu[d];
      for (int j = 0; j < P.m; ++j) for (int d = 0; d < 256; ++d) F[(3 + j) * 256 + d] = P.U[j][d];
      meta[g8 * 4] = P.guard; meta[g8 * 4 + 1] = P.m; meta[g8 * 4 + 2] = P.has_mu; }
    if (!kpk) throw std::runtime_error("K stream must be packed");
    gf = to_dev(Q, f.data(), f.size()); gci = to_dev(Q, ci.data(), ci.size()); gmeta = to_dev(Q, meta.data(), meta.size());
    kp = E.dev<uint32_t>((size_t)4 * max_ctx * 40 + 64); kps = E.dev<float>((size_t)4 * max_ctx + 64);
    if (vpk) { vp = E.dev<uint32_t>((size_t)4 * max_ctx * 40 + 64); vps = E.dev<float>((size_t)4 * max_ctx + 64); }
    else { v8 = E.dev<int8_t>((size_t)4 * max_ctx * 256); vs = E.dev<float>((size_t)4 * max_ctx + 64); }
    if (sh->mir) { mk = E.dev<uint16_t>((size_t)4 * sh->G * 4096); mv = E.dev<uint16_t>((size_t)4 * sh->G * 4096); }
  }
};

// ---- ESIMD kernels (default; E.fast).  Helpers of fast.hpp: per lane exactly the scalar operations, in the same order. ----
namespace ak {
using namespace fx;
YXI float v_s256(const vf* t) { float s = 0.f; unroll<8>([&](auto wi) { constexpr int w = decltype(wi)::value; s = s + v_xtree32(t[2 * w], t[2 * w + 1]); }); return s; }
YXI float tree16v(vf a) {                                                         // same order as tree16
  simd<float, 8> a0 = a.template select<8, 1>(0), a1 = a.template select<8, 1>(8); simd<float, 8> b = a0 + a1;
  simd<float, 4> b0 = b.template select<4, 1>(0), b1 = b.template select<4, 1>(4); simd<float, 4> c = b0 + b1;
  simd<float, 2> c0 = c.template select<2, 1>(0), c1 = c.template select<2, 1>(2); simd<float, 2> d = c0 + c1;
  const float d0 = d[0], d1 = d[1]; return d0 + d1;
}
// butterflies of kvx_rotate on 256 values (x[r] lane l = element 16 r + l), stages h = 1..128, (a + c, a - c), a = lower index
YXI void v_fwht256(vf* x) {
  unroll<16>([&](auto ri) { v_fwht_lanes(x[decltype(ri)::value]); });      // the four stages inside a register, see v_fwht512
  unroll<4>([&](auto si) { constexpr int d = 1 << decltype(si)::value;
    unroll<16>([&](auto pi) { constexpr int p = decltype(pi)::value;
      if constexpr (!(p & d)) { const vf a = x[p], b = x[p | d]; x[p] = a + b; x[p | d] = a - b; } }); });
}
YXI void v_rot_fwd(vf* x, const float* sg) {
  unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; x[k] = x[k] * block_load<float, 16>(sg + 16 * k); });
  v_fwht256(x);
  unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; x[k] = x[k] * 0.0625f; });
}
YXI void v_norm_rope(vf* x, const float* w, const float* cs, float eps) {
  vf t[16]; unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; t[k] = x[k] * x[k]; });
  const vf r = v_rsqrt(vdiv(vf(v_s256(t)), vf(256.f)) + eps);
  unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; x[k] = v_rbf((x[k] * r) * (1.f + block_load<float, 16>(w + 16 * k))); });
  unroll<2>([&](auto ii) { constexpr int i = decltype(ii)::value; const vf a = x[i], b = x[i + 2], c = block_load<float, 16>(cs + 16 * i), s = block_load<float, 16>(cs + 32 + 16 * i);
    x[i] = v_rbf(v_rbf(a * c) + v_rbf(flt(bits(b) ^ 0x80000000u) * s)); x[i + 2] = v_rbf(v_rbf(b * c) + v_rbf(a * s)); });
}
// trellis states of the 16 values e = 16*QC + lane of one chain (w: its 10 payload words; word 10 may be read, its bits are never used)
template <int QC> YXI vu tb_states16(const uint32_t* w) {
  vu a(16u * QC, 1u); a = a * 5u; vu shr(24u);
  if constexpr (QC == 0) { a = (max)(a, vu(3u)) - 3u; shr.template select<1, 1>(0) = 27u; } else a = a - 3u;       // window [5e-3, 5e+5); first value of a chain: 5 bits
  const vu wo = (a >> 5) << 2, off = a & 31u;
  const vu w0 = gather<uint32_t, 16>(w, wo), w1 = gather<uint32_t, 16>(w, wo + 4u);
  return ((w0 << off) | ((w1 >> (31u - off)) >> 1)) >> shr;
}
struct Args {
  const int64_t *aq, *ak, *av; const float *tq, *tk, *tv, *sq, *sk, *sv, *us_in, *S, *qn, *kn, *rope, *sg, *mult, *gf, *si; const int *gci, *gmeta;
  float *q16, *gate, *y, *y2, *enc_r, *enc_c, *enc_e, *pm, *pl, *pacc, *kps, *vps, *vs, *us, *alpha; uint8_t *enc_st, *vbp; int* vxi; uint32_t *kp, *vp; int8_t *v8, *qx; int32_t* Aw;
  int M, MR, pos0, max_ctx; float eps; bool vpk;
  const uint16_t *mk, *mv; float* sc; uint32_t *pa, *qa; int G, NP, PS, NQG; bool mir; int dbg; };

// 1. post: one thread per (row, 512-block of the projection outputs)
inline void post(sycl::queue& Q, const Args P) {
  Q.parallel_for(sycl::range<1>(P.M * 28), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int tok = it[0] / 28, blk = it[0] % 28, pos = P.pos0 + tok; vf x[32];
    const int64_t* acc = blk < 24 ? P.aq + (size_t)tok * A_NQ + blk * 512 : blk < 26 ? P.ak + (size_t)tok * A_NKV + (blk - 24) * 512 : P.av + (size_t)tok * A_NKV + (blk - 26) * 512;
    const float* tau = blk < 24 ? P.tq + blk * 512 : blk < 26 ? P.tk + (blk - 24) * 512 : P.tv + (blk - 26) * 512;
    const float* so = blk < 24 ? P.sq + blk * 512 : blk < 26 ? P.sk + (blk - 24) * 512 : P.sv + (blk - 26) * 512;
    v_yrot_n(x, acc, tau, vf(P.us_in[tok]), v_rfac(P.S + tok * SST, P.eps)); v_fwht512(x);
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = v_rbf((x[r] * K512) * block_load<float, 16>(so + r * 16)); });
    const float* cs = P.rope + (size_t)pos * 64;
    if (blk < 24) { float* go = P.gate + (size_t)(tok * 24 + blk) * 256; float* qo = P.q16 + (size_t)(tok * 24 + blk) * 256;
      unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; block_store<float, 16>(go + 16 * k, x[16 + k]); });
      v_norm_rope(x, P.qn, cs, P.eps); v_rot_fwd(x, P.sg);
      unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; x[k] = v_rbf(x[k]); block_store<float, 16>(qo + 16 * k, x[k]); });
      if (P.mir) { const int u = tok * 6 + blk % 6; uint32_t* qo2 = P.qa + (size_t)((blk / 6) * P.NQG + u / 8) * 1024 + (u % 8) * 8;
        unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; vu b = bits(x[k]) >> 16;
          const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); block_store<uint32_t, 8>(qo2 + k * 64, lo | (hi << 16)); }); }
      return; }
    const int kind = blk >= 26;
    unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value; vf* xh = x + 16 * hh; const int g8 = kind * 4 + (blk - 24 - 2 * kind) * 2 + hh;
      if (!kind) v_norm_rope(xh, P.kn, cs, P.eps);
      v_rot_fwd(xh, P.sg);
      vf am = 0.f; unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; am = (max)(am, flt(bits(xh[k]) & 0x7fffffffu)); });
      const vf sc = vdiv(vf(hmax<float>(am)), vf(127.f)); const vm nz = sc > 0.f;
      unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; vf q = 0.f; q.merge(rnde<float, 16>(vdiv(xh[k], sc)), nz); q = (max)(q, vf(-127.f)); q = (min)(q, vf(127.f)); xh[k] = q * sc; });
      float* er = P.enc_r + (size_t)(tok * 8 + g8) * 256; float* ec = P.enc_c + (tok * 8 + g8) * 4; const int* mt = P.gmeta + g8 * 4; const float* G = P.gf + (size_t)g8 * 1280;
      if (pos < mt[0]) ec[3] = 1.f;                                                                  // guard row: no trellis
      else {
        if (mt[2]) unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; xh[k] = xh[k] - block_load<float, 16>(G + 512 + 16 * k); });
        ec[0] = 0.f; ec[1] = 0.f; vf t[16];
        for (int j = 0; j < mt[1]; ++j) { const float* U = G + (3 + j) * 256;
          unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; t[k] = xh[k] * block_load<float, 16>(U + 16 * k); });
          const vf cv = v_rf16(vf(v_s256(t))); const float c = cv[0]; ec[j] = c;
          unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; xh[k] = xh[k] - cv * block_load<float, 16>(U + 16 * k); }); }
        unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; t[k] = xh[k] * xh[k]; });
        const vf rv = vsqrt(vdiv(vf(v_s256(t)), vf(256.f))); float rms = rv[0]; if (rms == 0.f) rms = 1.f;
        ec[2] = rms; ec[3] = 0.f; }
      unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; block_store<float, 16>(er + 16 * k, xh[k]); }); }); });
}
// 2. trellis paths: one thread per (row, group, scale, chain); the 256 path costs live in 16 registers (state = 16 v + lane)
inline void viterbi(sycl::queue& Q, const Args P) {
  Q.parallel_for(sycl::range<1>(P.M * 8 * A_NM * 4), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int ch = it[0] & 3, mi = (it[0] >> 2) % A_NM, tg = it[0] / (4 * A_NM), g8 = tg & 7;
    const float* ec = P.enc_c + tg * 4; if (ec[3] != 0.f) return;
    const float* r = P.enc_r + (size_t)tg * 256 + ch * 64; const int* ci = P.gci + g8 * 256; const float* cdec = P.gf + (size_t)g8 * 1280;
    const vf s0 = v_rf16(vf(ec[2] * P.mult[mi])); float s = s0[0]; if (s == 0.f) s = 1.f; const vf sv(s);
    int* xs = P.vxi + (size_t)it[0] * 64; uint8_t* bp = P.vbp + (size_t)it[0] * 1024;
    unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value;
      vf xf = rnde<float, 16>(vdiv(block_load<float, 16>(r + 16 * k), sv) * 256.f); xf = (max)(xf, vf(-2048.f)); xf = (min)(xf, vf(2048.f)); block_store<int, 16>(xs + 16 * k, vi(xf)); });
    vi cost[16]; unroll<16>([&](auto vi_) { cost[decltype(vi_)::value] = 0x40000000; }); cost[0].template select<1, 1>(0) = 0;
    for (int t = 0; t < 64; ++t) {
      vi best = cost[0], jv = 0;                                                  // per lane (u = lane & 7, parity = lane >> 3): min over v and the first v reaching it
      unroll<15>([&](auto vi_) { constexpr int v = decltype(vi_)::value + 1; const vm lt = cost[v] < best; best.merge(cost[v], lt); jv.merge(vi(v), lt); });
      simd<int, 8> c0 = best.template select<8, 1>(0), c1 = best.template select<8, 1>(8), j0 = jv.template select<8, 1>(0), j1 = jv.template select<8, 1>(8);
      j0 = j0 * 2; j1 = j1 * 2 + 1;                                               // j = 2 v + parity; smallest j among the minima
      const simd_mask<8> tk = (c1 < c0) | ((c1 == c0) & (j1 < j0)); c0.merge(c1, tk); j0.merge(j1, tk);
      simd<uint8_t, 16> jb = 0; jb.template select<8, 1>(0) = simd<uint8_t, 8>(j0); block_store<uint8_t, 16>(bp + t * 16, jb);
      const vi xv(xs[t]);
      unroll<16>([&](auto vi_) { constexpr int v = decltype(vi_)::value; const int mn = c0[v >> 1]; const vi d = xv - block_load<int, 16>(ci + 16 * v); cost[v] = d * d + mn; });
    }
    vi best = cost[0], jv = 0;
    unroll<15>([&](auto vi_) { constexpr int v = decltype(vi_)::value + 1; const vm lt = cost[v] < best; best.merge(cost[v], lt); jv.merge(vi(v), lt); });
    const int mn = hmin<int>(best); vi sidx = jv * 16 + vi(0, 1); sidx.merge(vi(1 << 20), best > mn);
    int stt = hmin<int>(sidx);                                                    // smallest state with minimal cost
    uint8_t* so = P.enc_st + ((size_t)tg * A_NM + mi) * 256 + ch * 64;
    for (int t = 63; t >= 0; --t) { so[t] = (uint8_t)stt; stt = ((int)bp[t * 16 + (stt >> 5)] << 3) | (stt >> 5); }
    vf dd[4];
    unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; const vu st = simd<uint8_t, 16>(block_load<uint8_t, 16>(so + 16 * k));
      const vf d = block_load<float, 16>(r + 16 * k) - gather<float, 16>(cdec, st << 2) * sv; dd[k] = d * d; });
    float* eo = P.enc_e + ((size_t)tg * A_NM + mi) * 8 + ch * 2; eo[0] = v_xtree32(dd[0], dd[1]); eo[1] = v_xtree32(dd[2], dd[3]); });
}
// 4. partial softmax of one (row, NG query heads of one KV head, split).  Keys are decoded once for the NG heads; a tile is
//    walked chain by chain (4 x 64 values) so that the accumulators of a pass stay in registers; o lives in pacc between tiles.
template <int NG> inline void attend(sycl::queue& Q, const Args P) {
  constexpr int NHG = A_NH / NG;
  Q.parallel_for(sycl::range<1>((size_t)P.M * NHG * A_SPL), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL { const int s = it[0] % A_SPL, th = it[0] / A_SPL, hd0 = (th % NHG) * NG, tok = th / NHG, kvh = hd0 / A_GQ;
    const int ln = P.pos0 + tok + 1, own = P.pos0 + tok, S = ln < A_LONG ? A_SHORT : A_SPL; if (s >= S) return;
    const int per = (ln + S - 1) / S, lo = s * per, hi = ln < lo + per ? ln : lo + per, phh = hi < ln - 1 ? hi : ln - 1, ph = lo > phh ? lo : phh;
    const bool has_self = lo <= ln - 1 && ln - 1 < lo + per; const size_t oi = ((size_t)tok * A_SPL + s) * A_NH + hd0;
    float m[NG], l[NG];
    if (!(ph > lo || has_self)) { unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; P.pm[oi + g] = -1e30f; P.pl[oi + g] = 0.f; }); return; }
    unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; m[g] = -1e30f; l[g] = 0.f; });
    const float* qv = P.q16 + (size_t)(tok * A_NH + hd0) * 256; float* po = P.pacc + oi * 256;
    const float* tabK = P.gf + (size_t)kvh * 1280 + 256; const float* GV = P.gf + (size_t)(4 + kvh) * 1280; const float* tabV = GV + 256;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const bool vpk = P.vpk;
    const float NINF = -std::numeric_limits<float>::infinity(); const vi iota(0, 1); bool first = true;
    for (int t0 = (lo / 32) * 32; t0 < ph; t0 += 32) { const int a = t0 > lo ? t0 : lo, b = t0 + 32 < ph ? t0 + 32 : ph; if (b <= a) continue;
      vf accA[NG], accB[NG]; unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; accA[g] = 0.f; accB[g] = 0.f; });
      unroll<4>([&](auto ci) { constexpr int c = decltype(ci)::value; vf q[NG][4];                    // prefix-key dots: 16-element chunks in order, XOR tree inside
        unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value; q[g][qq] = block_load<float, 16>(qv + g * 256 + c * 64 + qq * 16); }); });
        for (int j = a; j < b; ++j) { const int i = j - t0; const uint32_t* w = P.kp + (r0 + j) * 40 + c * 10; const vm mA = iota == i, mB = iota == (i - 16);
          unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value; const vf kv = gather<float, 16>(tabK, tb_states16<qq>(w) << 2);
            unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; const float t = tree16v(q[g][qq] * kv); accA[g].merge(accA[g] + t, mA); accB[g].merge(accB[g] + t, mB); }); }); } });
      const vf ksA = block_load<float, 16>(P.kps + r0 + t0) * 0.0625f, ksB = block_load<float, 16>(P.kps + r0 + t0 + 16) * 0.0625f;
      const vf vsA = block_load<float, 16>(vsc + r0 + t0), vsB = block_load<float, 16>(vsc + r0 + t0 + 16);
      const vm inA = (iota >= (a - t0)) & (iota < (b - t0)), inB = (iota >= (a - t0 - 16)) & (iota < (b - t0 - 16));
      vf pfA[NG], pfB[NG]; float cf[NG];
      unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value;
        vf scA = accA[g] * ksA, scB = accB[g] * ksB; scA.merge(vf(NINF), !inA); scB.merge(vf(NINF), !inB);
        const float mx = hmax<float>((max)(scA, scB)), x = m[g] > mx ? m[g] : mx, y0 = x * A_LOG2E;
        const vf cv = v_expf(vf(((m[g] - x) * A_LOG2E) * A_LN2)); const float c = cv[0]; cf[g] = c; m[g] = x;
        vf pA = v_expf((scA * A_LOG2E - y0) * A_LN2), pB = v_expf((scB * A_LOG2E - y0) * A_LN2);       // masked keys: exp2f(-inf) = +0
        simd<float, 8> eA = pA.template select<8, 2>(0), oA = pA.template select<8, 2>(1), eB = pB.template select<8, 2>(0), oB = pB.template select<8, 2>(1);
        simd<float, 8> sA = eA + oA, sB = eB + oB;                                                   // [4 jj + r] = p[8jj+2r] + p[8jj+2r+1]
        simd<float, 4> s0 = sA.template select<4, 1>(0), s1 = sA.template select<4, 1>(4), s2 = sB.template select<4, 1>(0), s3 = sB.template select<4, 1>(4);
        simd<float, 4> t = ((s0 + s1) + s2) + s3; const float u0 = t[0], u1 = t[1], u2 = t[2], u3 = t[3];
        l[g] = l[g] * c + ((u0 + u1) + (u2 + u3));
        pfA[g] = v_rbf(pA * vsA); pfB[g] = v_rbf(pB * vsB); });                                      // BF16 rounding of p*vs
      unroll<4>([&](auto ci) { constexpr int c = decltype(ci)::value; vf o[NG][4];
        unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value;
          if (first) o[g][qq] = 0.f; else o[g][qq] = block_load<float, 16>(po + g * 256 + c * 64 + qq * 16);
          o[g][qq] = o[g][qq] * cf[g]; }); });
        for (int j = a; j < b; ++j) { const int i = j - t0; const vm mA = iota == i, mB = iota == (i - 16); float pf[NG];        // keys in slot order
          unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; vf tA(NINF), tB(NINF); tA.merge(pfA[g], mA); tB.merge(pfB[g], mB); pf[g] = hmax<float>((max)(tA, tB)); });
          if (vpk) { const uint32_t* w = P.vp + (r0 + j) * 40 + c * 10;
            unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value; const vf vv = gather<float, 16>(tabV, tb_states16<qq>(w) << 2);
              unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; o[g][qq] = o[g][qq] + vv * pf[g]; }); }); }
          else { const int8_t* w = P.v8 + (r0 + j) * 256 + c * 64;
            unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value; const vf vv = vi(simd<int8_t, 16>(block_load<int8_t, 16>(w + 16 * qq)));
              unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; o[g][qq] = o[g][qq] + vv * pf[g]; }); }); } }
        unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; unroll<4>([&](auto qi) { constexpr int qq = decltype(qi)::value; block_store<float, 16>(po + g * 256 + c * 64 + qq * 16, o[g][qq]); }); }); });
      first = false;
    }
    if (has_self) { simd<float, 4> p4[NG]; unroll<NG>([&](auto gi) { p4[decltype(gi)::value] = 0.f; });
      unroll<16>([&](auto ci) { constexpr int cc = decltype(ci)::value; const vf kv = gather<float, 16>(tabK, tb_states16<cc % 4>(P.kp + (r0 + own) * 40 + (cc / 4) * 10) << 2);
        unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; vf pr = block_load<float, 16>(qv + g * 256 + 16 * cc) * kv;
          simd<float, 4> e0 = pr.template select<4, 2>(0), e1 = pr.template select<4, 2>(1), e2 = pr.template select<4, 2>(8), e3 = pr.template select<4, 2>(9);      // e = 2r, 2r+1, 8+2r, 9+2r
          p4[g] = p4[g] + e0; p4[g] = p4[g] + e1; p4[g] = p4[g] + e2; p4[g] = p4[g] + e3; }); });
      float cf[NG], pv[NG]; const float kso = P.kps[r0 + own] * 0.0625f, vso = vsc[r0 + own];
      unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; const float q0 = p4[g][0], q1 = p4[g][1], q2 = p4[g][2], q3 = p4[g][3];
        const float sc = ((q0 + q1) + (q2 + q3)) * kso, mn = m[g] > sc ? m[g] : sc;
        vf ar((m[g] - mn) * A_LOG2E); ar.template select<1, 1>(1) = (sc - mn) * A_LOG2E; const vf ev = v_expf(ar * A_LN2); const float c = ev[0], p = ev[1];
        m[g] = mn; l[g] = l[g] * c + p; cf[g] = c; pv[g] = p * vso; });
      unroll<16>([&](auto ci) { constexpr int cc = decltype(ci)::value; vf vv;
        if (vpk) vv = gather<float, 16>(tabV, tb_states16<cc % 4>(P.vp + (r0 + own) * 40 + (cc / 4) * 10) << 2);
        else vv = vi(simd<int8_t, 16>(block_load<int8_t, 16>(P.v8 + (r0 + own) * 256 + 16 * cc)));
        unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; vf o = 0.f; if (!first) o = block_load<float, 16>(po + g * 256 + 16 * cc);
          block_store<float, 16>(po + g * 256 + 16 * cc, o * cf[g] + vv * pv[g]); }); }); }
    if (vpk) unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; unroll<16>([&](auto ci) { constexpr int cc = decltype(ci)::value;
      block_store<float, 16>(po + g * 256 + 16 * cc, block_load<float, 16>(po + g * 256 + 16 * cc) + block_load<float, 16>(GV + 512 + 16 * cc) * l[g]); }); });
    unroll<NG>([&](auto gi) { constexpr int g = decltype(gi)::value; P.pm[oi + g] = m[g]; P.pl[oi + g] = l[g]; }); });
}
// 4m. mirror path, scores + softmax weights: one thread per (KV head, QS groups of 8 queries, part of A_PS keys).
// Query u = row * 6 + (head % 6) of a KV head sits in group u / 8, slot u % 8.  Per 16 keys: 16 XMX products (bf16 x bf16 -> float) per
// query group; the maximum of the part, then p = exp(score - max), the sum of p and the weights rbf(p * vs) as bf16 for the value products.
template <int QS> inline void score(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQS = (NQ + QS - 1) / QS, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::range<1>((size_t)4 * NQS * NPp), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL {
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int s = it[0] % NPp, rr = it[0] / NPp, qs0 = (rr % NQS) * QS, kvh = rr / NQS, nq = NQ - qs0 < QS ? NQ - qs0 : QS;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + qs0) * 1024;
    float* SC = P.sc + (size_t)(kvh * P.NQG + qs0) * P.G * 128; uint32_t* PA = P.pa + (size_t)(kvh * P.NQG + qs0) * P.G * 64;
    for (int g = g0; g < gE; ++g) { const int* KB = KB0 + (size_t)g * 2048;
      simd<float, 128> acc[QS]; unroll<QS>([&](auto qi) { acc[decltype(qi)::value] = 0.f; });
      unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
        B.template select<64, 1>(0) = block_load<int, 64>(KB + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(KB + c * 128 + 64);
        unroll<QS>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) { const simd<int, 64> A = block_load<int, 64>(QA + (q * 16 + c) * 64);
          acc[q] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc[q], B, A); } }); });
      const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f; const bool msk = 16 * g + 15 > P.pos0;
      unroll<QS>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) {
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc[q].template select<16, 1>(16 * m); a = a * ks;
          if (msk) { const int lim = P.pos0 + ((qs0 + q) * 8 + m) / 6 - 16 * g; a.merge(vf(NINF), iota > lim); }
          acc[q].template select<16, 1>(16 * m) = a; });
        block_store<float, 64>(SC + ((size_t)q * P.G + g) * 128, acc[q].template select<64, 1>(0)); block_store<float, 64>(SC + ((size_t)q * P.G + g) * 128 + 64, acc[q].template select<64, 1>(64)); } }); }
    float mx[QS][8];                                                                                // maximum of the part per query: second sweep over the stored scores
    unroll<QS>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) { vf vm8[8]; unroll<8>([&](auto mi) { vm8[decltype(mi)::value] = NINF; });
      for (int g = g0; g < gE; ++g) { const float* sp = SC + ((size_t)q * P.G + g) * 128;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vm8[m] = (max)(vm8[m], block_load<float, 16>(sp + 16 * m)); }); }
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; mx[q][m] = hmax<float>(vm8[m]); }); } });
    vf ls[QS][8]; unroll<QS>([&](auto qi) { unroll<8>([&](auto mi) { ls[decltype(qi)::value][decltype(mi)::value] = 0.f; }); });
    for (int g = g0; g < gE; ++g) { const vf vsv = block_load<float, 16>(vsc + r0 + 16 * g);
      unroll<QS>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) { const float* sp = SC + ((size_t)q * P.G + g) * 128; uint32_t* po = PA + ((size_t)q * P.G + g) * 64;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const vf e = v_expf(block_load<float, 16>(sp + 16 * m) - mx[q][m]); ls[q][m] = ls[q][m] + e;
          vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); block_store<uint32_t, 8>(po + 8 * m, lo | (hi << 16)); }); } }); }
    unroll<QS>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) { unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value;
      const int u = (qs0 + q) * 8 + m, row = u / 6; if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6;
        P.pm[oi] = mx[q][m]; P.pl[oi] = tree16v(ls[q][m]); } }); } }); });
}
// 4m. scores, variant with the queries in registers: one thread per (KV head, group of 8 queries, part).  The 16 query operands (64
// registers) are loaded once, the running maximum stays in registers and the scores / weights move in 256-byte blocks, so that per
// 16 keys the thread sends 32 loads for the keys and 5 other messages (score<QS>: about 75).  Same values as score<QS>.
inline void score1(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::range<1>((size_t)4 * NQ * NPp), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL {
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int s = it[0] % NPp, rr = it[0] / NPp, qg = rr % NQ, kvh = rr / NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + qg) * 1024;
    float* SC = P.sc + (size_t)(kvh * P.NQG + qg) * P.G * 128; uint32_t* PA = P.pa + (size_t)(kvh * P.NQG + qg) * P.G * 64;
    simd<int, 64> A[16]; unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; A[c] = block_load<int, 64>(QA + c * 64); });
    simd<float, 128> vmax = NINF;
    for (int g = g0; g < gE; ++g) { const int* KB = KB0 + (size_t)g * 2048; simd<float, 128> acc = 0.f;
      unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
        B.template select<64, 1>(0) = block_load<int, 64>(KB + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(KB + c * 128 + 64);
        acc = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc, B, A[c]); });
      const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f; const bool msk = 16 * g + 15 > P.pos0;
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc.template select<16, 1>(16 * m); a = a * ks;
        if (msk) { const int lim = P.pos0 + (qg * 8 + m) / 6 - 16 * g; a.merge(vf(NINF), iota > lim); }
        acc.template select<16, 1>(16 * m) = a; });
      vmax = (max)(vmax, acc);
      block_store<float, 64>(SC + (size_t)g * 128, acc.template select<64, 1>(0)); block_store<float, 64>(SC + (size_t)g * 128 + 64, acc.template select<64, 1>(64)); }
    vf mx[8], ls[8]; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const vf v = vmax.template select<16, 1>(16 * m); mx[m] = hmax<float>(v); ls[m] = 0.f; });
    for (int g = g0; g < gE; ++g) { const vf vsv = block_load<float, 16>(vsc + r0 + 16 * g); simd<float, 128> sv; simd<uint32_t, 64> out;
      sv.template select<64, 1>(0) = block_load<float, 64>(SC + (size_t)g * 128); sv.template select<64, 1>(64) = block_load<float, 64>(SC + (size_t)g * 128 + 64);
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const vf e = v_expf(vf(sv.template select<16, 1>(16 * m)) - mx[m]); ls[m] = ls[m] + e;
        vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); out.template select<8, 1>(8 * m) = lo | (hi << 16); });
      block_store<uint32_t, 64>(PA + (size_t)g * 64, out); }
    unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
      if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float m1 = mx[m][0]; P.pm[oi] = m1; P.pl[oi] = tree16v(ls[m]); } }); });
}
#ifndef YX_ARG
#define YX_ARG 4
#endif
constexpr int A_RG = YX_ARG;      // groups of 16 keys staged in SLM per round by score16 / vacc16 (2 barriers per round)
// 4m. scores, work-group variant (batched queries): a work-group of 16 threads = 16 groups of 8 queries of one (KV head, part).  For each
// group of 16 keys the work-group copies the keys (16 dim chunks, 8 KiB) to SLM once -- thread c loads chunk c -- and every thread
// multiplies them with ITS queries, whose 16 operands stay in registers (64).  The keys are read from global memory once per 128
// queries instead of once per 16.  Values, order and results are those of score<QS>.
inline void score16(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQW = (NQ + 15) / 16, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQW * NPp * 16, 16), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    slm_init<A_RG * 8192>();
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0);
    // dispatch order: the work-groups that use the same keys (KV head, part) are neighbours, so that the part is read from memory once and then from the cache
    const int rr = wg / NQW, s = rr % NPp, qg = (wg % NQW) * 16 + lid, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * 1024;
    float* SC = P.sc + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 128; uint32_t* PA = P.pa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 64;
    simd<int, 64> A[16]; unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; A[c] = block_load<int, 64>(QA + c * 64); });
    simd<float, 128> vmax = NINF;
    const bool nomul = P.dbg & 1;
    for (int gb = g0; gb < gE; gb += A_RG) { const int ng = gE - gb < A_RG ? gE - gb : A_RG;        // a round: A_RG groups of 16 keys staged at once (2 barriers per round)
      for (int r = 0; r < ng; ++r) { const int* KB = KB0 + (size_t)(gb + r) * 2048 + lid * 128;
        slm_block_store<int, 64>(r * 8192 + lid * 512, block_load<int, 64>(KB)); slm_block_store<int, 64>(r * 8192 + lid * 512 + 256, block_load<int, 64>(KB + 64)); }
      barrier();
      if (on) for (int r = 0; r < ng; ++r) { const int g = gb + r; simd<float, 128> acc = 0.f;
        if (!nomul) unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(r * 8192 + c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(r * 8192 + c * 512 + 256);
          acc = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc, B, A[c]); });
        const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f; const bool msk = 16 * g + 15 > P.pos0;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc.template select<16, 1>(16 * m); a = a * ks;
          if (msk) { const int lim = P.pos0 + (qg * 8 + m) / 6 - 16 * g; a.merge(vf(NINF), iota > lim); }
          acc.template select<16, 1>(16 * m) = a; });
        vmax = (max)(vmax, acc);
        block_store<float, 64>(SC + (size_t)g * 128, acc.template select<64, 1>(0)); block_store<float, 64>(SC + (size_t)g * 128 + 64, acc.template select<64, 1>(64)); }
      barrier(); }
    if (!on) return;
    if (P.dbg & 2) { unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6; if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; P.pm[oi] = 0.f; P.pl[oi] = 1.f; } }); return; }
    vf mx[8], ls[8]; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const vf v = vmax.template select<16, 1>(16 * m); mx[m] = hmax<float>(v); ls[m] = 0.f; });
    for (int g = g0; g < gE; ++g) { const vf vsv = block_load<float, 16>(vsc + r0 + 16 * g); simd<float, 128> sv; simd<uint32_t, 64> out;
      sv.template select<64, 1>(0) = block_load<float, 64>(SC + (size_t)g * 128); sv.template select<64, 1>(64) = block_load<float, 64>(SC + (size_t)g * 128 + 64);
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const vf e = v_expf(vf(sv.template select<16, 1>(16 * m)) - mx[m]); ls[m] = ls[m] + e;
        vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); out.template select<8, 1>(8 * m) = lo | (hi << 16); });
      block_store<uint32_t, 64>(PA + (size_t)g * 64, out); }
    unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
      if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float m1 = mx[m][0]; P.pm[oi] = m1; P.pl[oi] = tree16v(ls[m]); } }); });
}
// 4m. weighted values, work-group variant: a work-group of 16 threads = 8 groups of 8 queries x 2 halves of the 16 dim chunks.  For each
// group of 16 keys the work-group copies the values (8 KiB, thread c loads chunk c) and the 8 weight operands to SLM once; every
// thread adds 8 XMX products to its 8 x 8 sums in registers.  Values, order and results are those of vacc<VQ, VC>.
inline void vacc16(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQ8 = (NQ + 7) / 8, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQ8 * NPp * 16, 16), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    slm_init<A_RG * (8192 + 2048)>();
    constexpr auto BF = xmx::dpas_argument_type::bf16;
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0), qi = lid >> 1, c0 = (lid & 1) * 8;
    const int rr = wg / NQ8, s = rr % NPp, qg = (wg % NQ8) * 8 + qi, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const int* VB0 = (const int*)P.mv + (size_t)kvh * P.G * 2048 + lid * 128; const int* PA = (const int*)P.pa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 64;
    simd<float, 128> o[8]; unroll<8>([&](auto ci) { o[decltype(ci)::value] = 0.f; });
    const bool nomul = P.dbg & 4;
    for (int gb = g0; gb < gE; gb += A_RG) { const int ng = gE - gb < A_RG ? gE - gb : A_RG;
      for (int r = 0; r < ng; ++r) { const int* VB = VB0 + (size_t)(gb + r) * 2048; const uint32_t rb = (uint32_t)r * (8192 + 2048);
        slm_block_store<int, 64>(rb + lid * 512, block_load<int, 64>(VB)); slm_block_store<int, 64>(rb + lid * 512 + 256, block_load<int, 64>(VB + 64));
        if (c0 == 0 && on) slm_block_store<int, 64>(rb + 8192 + qi * 256, block_load<int, 64>(PA + (size_t)(gb + r) * 64)); }
      barrier();
      if (on && !nomul) for (int r = 0; r < ng; ++r) { const uint32_t rb = (uint32_t)r * (8192 + 2048); const simd<int, 64> A = slm_block_load<int, 64>(rb + 8192 + qi * 256);
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(rb + (c0 + c) * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(rb + (c0 + c) * 512 + 256);
          o[c] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(o[c], B, A); }); }
      barrier(); }
    if (!on) return;
    const float* MU = P.gf + (size_t)(4 + kvh) * 1280 + 512 + c0 * 16; const bool vpk = P.vpk;
    unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
      if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float l = P.pl[oi]; float* po = P.pacc + oi * 256 + c0 * 16;
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; vf v = o[c].template select<16, 1>(16 * m); if (vpk) v = v + block_load<float, 16>(MU + 16 * c) * l;
          block_store<float, 16>(po + 16 * c, v); }); } }); });
}
// 4f. EXPERIMENT (YX_ATTN_FA=1): scores, online softmax and weighted values fused in one kernel; no score / weight buffers in
// global memory.  A work-group of 48 threads serves 16 groups of 8 queries of one (KV head, part): threads 0..15 compute the scores
// of one query group each (queries in registers), threads 16..47 the weighted values (2 threads per query group, 8 dim chunks each).
// Per round of FA_RG groups of 16 keys: the K and V tiles go to SLM (threads 0..31 copy one chunk each); the score threads write
// the weights (bf16) and the rescale factors to SLM; the value threads apply them.  Online softmax per query and group of 16 keys:
// m' = max(m, max s), c = exp(m - m'), p = exp(s - m'), l = l c + sum p, o = o c + sum p v  (c = 1 exactly when the maximum
// does not move: then nothing is rescaled).  NOT bit-identical to the default path (score16 / vacc16 use the maximum of the part).
constexpr int FA_RG = 4;
inline void fa48(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQW = (NQ + 15) / 16, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQW * NPp * 48, 48), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    constexpr int KO = 0, VO = FA_RG * 8192, PO = 2 * FA_RG * 8192, CO = PO + 16 * FA_RG * 256;       // SLM: K tiles, V tiles, weights [slot][round group][256], factors [slot][round group][64]
    slm_init<CO + 16 * FA_RG * 64>();
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0); const bool sc_thr = lid < 16;
    const int slot = sc_thr ? lid : (lid - 16) >> 1, c0 = sc_thr ? 0 : ((lid - 16) & 1) * 8;
    const int rr = wg / NQW, s = rr % NPp, qg = (wg % NQW) * 16 + slot, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* VB0 = (const int*)P.mv + (size_t)kvh * P.G * 2048;
    const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * 1024;
    simd<float, 128> o[8]; float mc[8], lc[8];       // o: the 8 sums of a value thread, or (bit-cast) the 16 query operands of a score thread -- one block of 64 registers for both roles
    if (sc_thr) { unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; o[c / 2].template bit_cast_view<int>().template select<64, 1>((c & 1) * 64) = block_load<int, 64>(QA + c * 64); });
      unroll<8>([&](auto mi) { mc[decltype(mi)::value] = NINF; lc[decltype(mi)::value] = 0.f; }); }
    else unroll<8>([&](auto ci) { o[decltype(ci)::value] = 0.f; });
    for (int gb = g0; gb < gE; gb += FA_RG) { const int ng = gE - gb < FA_RG ? gE - gb : FA_RG;
      if (lid < 32) { const int* src = (lid < 16 ? KB0 : VB0) + (size_t)gb * 2048 + (lid & 15) * 128; const uint32_t dst = (lid < 16 ? KO : VO) + (lid & 15) * 512;
        for (int r = 0; r < ng; ++r) { slm_block_store<int, 64>(dst + r * 8192, block_load<int, 64>(src + (size_t)r * 2048)); slm_block_store<int, 64>(dst + r * 8192 + 256, block_load<int, 64>(src + (size_t)r * 2048 + 64)); } }
      barrier();
      if (sc_thr && on) for (int r = 0; r < ng; ++r) { const int g = gb + r; simd<float, 128> acc = 0.f;
        unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(KO + r * 8192 + c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(KO + r * 8192 + c * 512 + 256);
          const simd<int, 64> Ac = o[c / 2].template bit_cast_view<int>().template select<64, 1>((c & 1) * 64);
          acc = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc, B, Ac); });
        const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f, vsv = block_load<float, 16>(vsc + r0 + 16 * g); const bool msk = 16 * g + 15 > P.pos0;
        simd<uint32_t, 64> out; vf cf = 1.f;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc.template select<16, 1>(16 * m); a = a * ks; vm dead = iota > 16;
          if (msk) { const int lim = P.pos0 + (qg * 8 + m) / 6 - 16 * g; dead = iota > lim; a.merge(vf(NINF), dead); }
          const float gm = hmax<float>(a), mn = mc[m] > gm ? mc[m] : gm; float c = 1.f;
          if (mn != mc[m]) { const vf cv = v_expf(vf(mc[m] - mn)); c = cv[0]; mc[m] = mn; }
          vf e = v_expf(a - mn); e.merge(vf(0.f), dead);
          lc[m] = lc[m] * c + tree16v(e); cf.template select<1, 1>(m) = c;
          vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); out.template select<8, 1>(8 * m) = lo | (hi << 16); });
        slm_block_store<uint32_t, 64>(PO + (slot * FA_RG + r) * 256, out); slm_block_store<float, 16>(CO + (slot * FA_RG + r) * 64, cf); }
      barrier();
      if (!sc_thr && on) for (int r = 0; r < ng; ++r) { const simd<int, 64> Aw = slm_block_load<int, 64>(PO + (slot * FA_RG + r) * 256); const vf cf = slm_block_load<float, 16>(CO + (slot * FA_RG + r) * 64);
        vm ne = cf != 1.f; if (ne.any()) unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const float c = cf[m];
          unroll<8>([&](auto ci) { constexpr int cc = decltype(ci)::value; vf t = o[cc].template select<16, 1>(16 * m); o[cc].template select<16, 1>(16 * m) = t * c; }); });
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(VO + r * 8192 + (c0 + c) * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(VO + r * 8192 + (c0 + c) * 512 + 256);
          o[c] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(o[c], B, Aw); }); }
      barrier(); }
    if (sc_thr && on) { unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
        if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; P.pm[oi] = mc[m]; P.pl[oi] = lc[m]; } });
      vf lv = 0.f; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; lv.template select<1, 1>(m) = lc[m]; }); slm_block_store<float, 16>(CO + slot * FA_RG * 64, lv); }
    barrier();                                                                                 // every thread of the group reaches this barrier
    if (!sc_thr && on) { const vf lv = slm_block_load<float, 16>(CO + slot * FA_RG * 64);            // sum of p of the 8 queries (for the mean of a packed V stream)
      const float* MU = P.gf + (size_t)(4 + kvh) * 1280 + 512 + c0 * 16; const bool vpk = P.vpk;
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
        if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float l = lv[m]; float* po = P.pacc + oi * 256 + c0 * 16;
          unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; vf v = o[c].template select<16, 1>(16 * m); if (vpk) v = v + block_load<float, 16>(MU + 16 * c) * l;
            block_store<float, 16>(po + 16 * c, v); }); } }); } });
}
// 4p. EXPERIMENTS (YX_ATTN_FA = 2..): the fused kernel as a template.  Same arithmetic, in the same order, as fa48 (so the results
// must be bit-identical to fa48; like fa48 they are NOT bit-identical to score16 / vacc16).
//   QW    query groups per work-group: QW score threads + 2 QW value threads (48 threads for QW = 16, 24 for QW = 8)
//   RG    groups of 16 keys per round
//   PIPE  0: the phases of fa48 (copy | barrier | scores | barrier | values | barrier: the score threads wait while the value
//            threads work and the other way round)
//         1: pipeline.  In phase p the score threads compute round p while the value threads consume round p - 1, with ONE barrier
//            per phase.  The online softmax lives entirely in the registers of a score thread (running maximum and sum per query),
//            so the only data that cross threads are the weights and rescale factors of a round, read one phase later.
//            Buffers (two of each, index = round & 1) and their owners in phase p:
//              K[p & 1]        read by the score threads           (written in phase p - 1 by the value threads)
//              K[(p + 1) & 1]  written by the value threads (keys of round p + 1)
//              W[p & 1], C[p & 1]  written by the score threads (weights, factors of round p)
//              V[p & 1]        written by the value threads (values of round p)
//              V[(p - 1) & 1], W[(p - 1) & 1], C[(p - 1) & 1]  read by the value threads
//            No buffer is read and written in the same phase.  R rounds take R + 2 barriers (3 R + 1 without the pipeline); every
//            thread of a group passes the same number of barriers because R depends only on the part.
//   RS    1: the value threads multiply their sums by the factors of the 8 queries unconditionally, as one vector (factor 1 where
//            the maximum did not move: exact); 0: only when a factor differs from 1, query by query (as fa48)
// The value threads copy the K and V tiles (QW = 16: one chunk of 16 dims each; QW = 8: one K and one V chunk each).
template <int QW, int RG, int PIPE, int RS> inline void fa_p(sycl::queue& Q, const Args P) {
  constexpr int WG = 3 * QW; const int NQ = (P.M * 6 + 7) / 8, NQW = (NQ + QW - 1) / QW, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQW * NPp * WG, WG), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    constexpr int NB = PIPE ? 2 : 1, KSZ = RG * 8192, WSZ = QW * RG * 256, CSZ = QW * RG * 64;
    constexpr int KO = 0, VO = NB * KSZ, PO = 2 * NB * KSZ, CO = PO + NB * WSZ, LO = CO + NB * CSZ;          // SLM: K, V tiles, weights, factors (NB of each), final sums
    slm_init<LO + QW * 64>();
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0); const bool sc_thr = lid < QW; const int vt = lid - QW;
    const int slot = sc_thr ? lid : vt >> 1, c0 = sc_thr ? 0 : (vt & 1) * 8;
    const int rr = wg / NQW, s = rr % NPp, qg = (wg % NQW) * QW + slot, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* VB0 = (const int*)P.mv + (size_t)kvh * P.G * 2048;
    const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * 1024;
    const bool cpK = !sc_thr && (QW == 16 ? vt < 16 : true), cpV = !sc_thr && (QW == 16 ? vt >= 16 : true); const int chunk = vt & 15;
    simd<float, 128> o[8]; float mc[8], lc[8];       // o: the 8 sums of a value thread, or (bit-cast) the 16 query operands of a score thread
    if (sc_thr) { unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; o[c / 2].template bit_cast_view<int>().template select<64, 1>((c & 1) * 64) = block_load<int, 64>(QA + c * 64); });
      unroll<8>([&](auto mi) { mc[decltype(mi)::value] = NINF; lc[decltype(mi)::value] = 0.f; }); }
    else unroll<8>([&](auto ci) { o[decltype(ci)::value] = 0.f; });
    auto copy = [&](const int* T0, uint32_t dst, int gb, int ng) SYCL_ESIMD_FUNCTION { const int* src = T0 + (size_t)gb * 2048 + chunk * 128; dst += chunk * 512;
      for (int r = 0; r < ng; ++r) { slm_block_store<int, 64>(dst + r * 8192, block_load<int, 64>(src + (size_t)r * 2048)); slm_block_store<int, 64>(dst + r * 8192 + 256, block_load<int, 64>(src + (size_t)r * 2048 + 64)); } };
    auto score = [&](int gb, int ng, uint32_t kb, uint32_t pb, uint32_t cb) SYCL_ESIMD_FUNCTION {
      for (int r = 0; r < ng; ++r) { const int g = gb + r; simd<float, 128> acc = 0.f;
        unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(kb + r * 8192 + c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(kb + r * 8192 + c * 512 + 256);
          const simd<int, 64> Ac = o[c / 2].template bit_cast_view<int>().template select<64, 1>((c & 1) * 64);
          acc = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc, B, Ac); });
        const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f, vsv = block_load<float, 16>(vsc + r0 + 16 * g); const bool msk = 16 * g + 15 > P.pos0;
        simd<uint32_t, 64> out; vf cf = 1.f;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc.template select<16, 1>(16 * m); a = a * ks; vm dead = iota > 16;
          if (msk) { const int lim = P.pos0 + (qg * 8 + m) / 6 - 16 * g; dead = iota > lim; a.merge(vf(NINF), dead); }
          const float gm = hmax<float>(a), mn = mc[m] > gm ? mc[m] : gm; float c = 1.f;
          if (mn != mc[m]) { const vf cv = v_expf(vf(mc[m] - mn)); c = cv[0]; mc[m] = mn; }
          vf e = v_expf(a - mn); e.merge(vf(0.f), dead);
          lc[m] = lc[m] * c + tree16v(e); cf.template select<1, 1>(m) = c;
          vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); out.template select<8, 1>(8 * m) = lo | (hi << 16); });
        slm_block_store<uint32_t, 64>(pb + (slot * RG + r) * 256, out); slm_block_store<float, 16>(cb + (slot * RG + r) * 64, cf); } };
    auto value = [&](int ng, uint32_t vb, uint32_t pb, uint32_t cb) SYCL_ESIMD_FUNCTION {
      for (int r = 0; r < ng; ++r) { const simd<int, 64> Aw = slm_block_load<int, 64>(pb + (slot * RG + r) * 256); const vf cf = slm_block_load<float, 16>(cb + (slot * RG + r) * 64);
        if constexpr (RS) { simd<float, 128> F; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const float c1 = cf[m]; F.template select<16, 1>(16 * m) = vf(c1); });
          unroll<8>([&](auto ci) { constexpr int cc = decltype(ci)::value; o[cc] = o[cc] * F; }); }
        else { vm ne = cf != 1.f; if (ne.any()) unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const float c = cf[m];
          unroll<8>([&](auto ci) { constexpr int cc = decltype(ci)::value; vf t = o[cc].template select<16, 1>(16 * m); o[cc].template select<16, 1>(16 * m) = t * c; }); }); }
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(vb + r * 8192 + (c0 + c) * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(vb + r * 8192 + (c0 + c) * 512 + 256);
          o[c] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(o[c], B, Aw); }); } };
    auto ngof = [&](int gb) SYCL_ESIMD_FUNCTION { return gE - gb < RG ? gE - gb : RG; };
    if constexpr (PIPE) { const int R = (gE - g0 + RG - 1) / RG;
      if (cpK) copy(KB0, KO, g0, ngof(g0));
      barrier();
      for (int p = 0; p <= R; ++p) { const int gb = g0 + p * RG; const uint32_t b = (uint32_t)(p & 1), bp = b ^ 1u;
        if (sc_thr) { if (p < R && on) score(gb, ngof(gb), KO + b * KSZ, PO + b * WSZ, CO + b * CSZ); }
        else { if (p + 1 < R && cpK) copy(KB0, KO + bp * KSZ, gb + RG, ngof(gb + RG));
          if (p < R && cpV) copy(VB0, VO + b * KSZ, gb, ngof(gb));
          if (p >= 1 && on) value(ngof(gb - RG), VO + bp * KSZ, PO + bp * WSZ, CO + bp * CSZ); }
        barrier(); } }
    else for (int gb = g0; gb < gE; gb += RG) { const int ng = ngof(gb);
      if (cpK) copy(KB0, KO, gb, ng);
      if (cpV) copy(VB0, VO, gb, ng);
      barrier();
      if (sc_thr && on) score(gb, ng, KO, PO, CO);
      barrier();
      if (!sc_thr && on) value(ng, VO, PO, CO);
      barrier(); }
    if (sc_thr && on) { unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
        if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; P.pm[oi] = mc[m]; P.pl[oi] = lc[m]; } });
      vf lv = 0.f; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; lv.template select<1, 1>(m) = lc[m]; }); slm_block_store<float, 16>(LO + slot * 64, lv); }
    barrier();                                                                                 // every thread of the group reaches this barrier
    if (!sc_thr && on) { const vf lv = slm_block_load<float, 16>(LO + slot * 64);                    // sum of p of the 8 queries (for the mean of a packed V stream)
      const float* MU = P.gf + (size_t)(4 + kvh) * 1280 + 512 + c0 * 16; const bool vpk = P.vpk;
      unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
        if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float l = lv[m]; float* po = P.pacc + oi * 256 + c0 * 16;
          unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; vf v = o[c].template select<16, 1>(16 * m); if (vpk) v = v + block_load<float, 16>(MU + 16 * c) * l;
            block_store<float, 16>(po + 16 * c, v); }); } }); } });
}
// 4o. EXPERIMENT (YX_ATTN_FA = 7): the two default kernels kept apart, with the online softmax of fa48 in the score kernel.  score16
// stores the float scores of a part (2 block stores per thread and group of 16 keys) and reads them back for the softmax once the
// maximum of the part is known (2 block loads).  Here the weights are written against the running maximum, in the same sweep, and
// the 8 rescale factors of a group go where the scores went (16 floats instead of 128); the value kernel applies them to its sums
// before it adds the group.  Same arithmetic as fa48 (bit-identical to it), not to score16 / vacc16.
inline void score16o(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQW = (NQ + 15) / 16, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQW * NPp * 16, 16), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    slm_init<A_RG * 8192>();
    constexpr auto BF = xmx::dpas_argument_type::bf16; const float NINF = -std::numeric_limits<float>::infinity();
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0);
    const int rr = wg / NQW, s = rr % NPp, qg = (wg % NQW) * 16 + lid, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const size_t r0 = (size_t)kvh * P.max_ctx; const float* vsc = P.vpk ? P.vps : P.vs; const vi iota(0, 1);
    const int* KB0 = (const int*)P.mk + (size_t)kvh * P.G * 2048; const int* QA = (const int*)P.qa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * 1024;
    float* SC = P.sc + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 128; uint32_t* PA = P.pa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 64;
    simd<int, 64> A[16]; unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; A[c] = block_load<int, 64>(QA + c * 64); });
    float mc[8], lc[8]; unroll<8>([&](auto mi) { mc[decltype(mi)::value] = NINF; lc[decltype(mi)::value] = 0.f; });
    for (int gb = g0; gb < gE; gb += A_RG) { const int ng = gE - gb < A_RG ? gE - gb : A_RG;
      for (int r = 0; r < ng; ++r) { const int* KB = KB0 + (size_t)(gb + r) * 2048 + lid * 128;
        slm_block_store<int, 64>(r * 8192 + lid * 512, block_load<int, 64>(KB)); slm_block_store<int, 64>(r * 8192 + lid * 512 + 256, block_load<int, 64>(KB + 64)); }
      barrier();
      if (on) for (int r = 0; r < ng; ++r) { const int g = gb + r; simd<float, 128> acc = 0.f;
        unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(r * 8192 + c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(r * 8192 + c * 512 + 256);
          acc = xmx::dpas<8, 8, float, float, int, int, BF, BF>(acc, B, A[c]); });
        const vf ks = block_load<float, 16>(P.kps + r0 + 16 * g) * 0.0625f, vsv = block_load<float, 16>(vsc + r0 + 16 * g); const bool msk = 16 * g + 15 > P.pos0;
        simd<uint32_t, 64> out; vf cf = 1.f;
        unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; vf a = acc.template select<16, 1>(16 * m); a = a * ks; vm dead = iota > 16;
          if (msk) { const int lim = P.pos0 + (qg * 8 + m) / 6 - 16 * g; dead = iota > lim; a.merge(vf(NINF), dead); }
          const float gm = hmax<float>(a), mn = mc[m] > gm ? mc[m] : gm; float c = 1.f;
          if (mn != mc[m]) { const vf cv = v_expf(vf(mc[m] - mn)); c = cv[0]; mc[m] = mn; }
          vf e = v_expf(a - mn); e.merge(vf(0.f), dead);
          lc[m] = lc[m] * c + tree16v(e); cf.template select<1, 1>(m) = c;
          vu b = v_f2bf(e * vsv); const simd<uint32_t, 8> lo = b.template select<8, 2>(0), hi = b.template select<8, 2>(1); out.template select<8, 1>(8 * m) = lo | (hi << 16); });
        block_store<uint32_t, 64>(PA + (size_t)g * 64, out); block_store<float, 16>(SC + (size_t)g * 128, cf); }
      barrier(); }
    if (!on) return;
    unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
      if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; P.pm[oi] = mc[m]; P.pl[oi] = lc[m]; } }); });
}
inline void vacc16o(sycl::queue& Q, const Args P) {
  const int NQ = (P.M * 6 + 7) / 8, NQ8 = (NQ + 7) / 8, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::nd_range<1>((size_t)4 * NQ8 * NPp * 16, 16), [=](sycl::nd_item<1> ndi) SYCL_ESIMD_KERNEL {
    constexpr int RB = 8192 + 2048 + 512;                                                    // per round group: V tile, 8 weight operands, 8 x 16 factors
    slm_init<A_RG * RB>();
    constexpr auto BF = xmx::dpas_argument_type::bf16;
    const int lid = (int)ndi.get_local_id(0), wg = (int)ndi.get_group(0), qi = lid >> 1, c0 = (lid & 1) * 8;
    const int rr = wg / NQ8, s = rr % NPp, qg = (wg % NQ8) * 8 + qi, kvh = rr / NPp; const bool on = qg < NQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const int* VB0 = (const int*)P.mv + (size_t)kvh * P.G * 2048 + lid * 128; const int* PA = (const int*)P.pa + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 64;
    const float* SC = P.sc + (size_t)(kvh * P.NQG + (on ? qg : 0)) * P.G * 128;
    simd<float, 128> o[8]; unroll<8>([&](auto ci) { o[decltype(ci)::value] = 0.f; });
    for (int gb = g0; gb < gE; gb += A_RG) { const int ng = gE - gb < A_RG ? gE - gb : A_RG;
      for (int r = 0; r < ng; ++r) { const int* VB = VB0 + (size_t)(gb + r) * 2048; const uint32_t rb = (uint32_t)r * RB;
        slm_block_store<int, 64>(rb + lid * 512, block_load<int, 64>(VB)); slm_block_store<int, 64>(rb + lid * 512 + 256, block_load<int, 64>(VB + 64));
        if (c0 == 0 && on) { slm_block_store<int, 64>(rb + 8192 + qi * 256, block_load<int, 64>(PA + (size_t)(gb + r) * 64)); slm_block_store<float, 16>(rb + 8192 + 2048 + qi * 64, block_load<float, 16>(SC + (size_t)(gb + r) * 128)); } }
      barrier();
      if (on) for (int r = 0; r < ng; ++r) { const uint32_t rb = (uint32_t)r * RB; const simd<int, 64> A = slm_block_load<int, 64>(rb + 8192 + qi * 256); const vf cf = slm_block_load<float, 16>(rb + 8192 + 2048 + qi * 64);
        vm ne = cf != 1.f; if (ne.any()) unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const float c = cf[m];
          unroll<8>([&](auto ci) { constexpr int cc = decltype(ci)::value; vf t = o[cc].template select<16, 1>(16 * m); o[cc].template select<16, 1>(16 * m) = t * c; }); });
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
          B.template select<64, 1>(0) = slm_block_load<int, 64>(rb + (c0 + c) * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(rb + (c0 + c) * 512 + 256);
          o[c] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(o[c], B, A); }); }
      barrier(); }
    if (!on) return;
    const float* MU = P.gf + (size_t)(4 + kvh) * 1280 + 512 + c0 * 16; const bool vpk = P.vpk;
    unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const int u = qg * 8 + m, row = u / 6;
      if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float l = P.pl[oi]; float* po = P.pacc + oi * 256 + c0 * 16;
        unroll<8>([&](auto ci) { constexpr int c = decltype(ci)::value; vf v = o[c].template select<16, 1>(16 * m); if (vpk) v = v + block_load<float, 16>(MU + 16 * c) * l;
          block_store<float, 16>(po + 16 * c, v); }); } }); });
}
// 4m. mirror path, weighted values: one thread per (KV head, VQ query groups, VC chunks of 16 dims, part).  Per 16 keys one XMX product per
// (query group, dim chunk); the mean of a packed V stream is added as mean * sum(p).
template <int VQ, int VC> inline void vacc(sycl::queue& Q, const Args P) {
  constexpr int NCS = 16 / VC; const int NQ = (P.M * 6 + 7) / 8, NQS = (NQ + VQ - 1) / VQ, NPp = (P.pos0 + P.M - 1) / P.PS + 1;
  Q.parallel_for(sycl::range<1>((size_t)4 * NQS * NCS * NPp), [=](sycl::id<1> it) SYCL_ESIMD_KERNEL {
    constexpr auto BF = xmx::dpas_argument_type::bf16;
    const int s = it[0] % NPp; int rr = it[0] / NPp; const int c0 = (rr % NCS) * VC; rr /= NCS; const int qs0 = (rr % NQS) * VQ, kvh = rr / NQS, nq = NQ - qs0 < VQ ? NQ - qs0 : VQ;
    const int last = P.pos0 + P.M - 1, g0 = s * (P.PS / 16), ge0 = g0 + P.PS / 16, gE = ge0 < last / 16 + 1 ? ge0 : last / 16 + 1;
    const int* VB0 = (const int*)P.mv + (size_t)kvh * P.G * 2048 + c0 * 128; const int* PA = (const int*)P.pa + (size_t)(kvh * P.NQG + qs0) * P.G * 64;
    simd<float, 128> o[VQ][VC]; unroll<VQ>([&](auto qi) { unroll<VC>([&](auto ci) { o[decltype(qi)::value][decltype(ci)::value] = 0.f; }); });
    for (int g = g0; g < gE; ++g) { const int* VB = VB0 + (size_t)g * 2048;
      simd<int, 64> A[VQ]; unroll<VQ>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) A[q] = block_load<int, 64>(PA + ((size_t)q * P.G + g) * 64); });
      unroll<VC>([&](auto ci) { constexpr int c = decltype(ci)::value; simd<int, 128> B;
        B.template select<64, 1>(0) = block_load<int, 64>(VB + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(VB + c * 128 + 64);
        unroll<VQ>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) o[q][c] = xmx::dpas<8, 8, float, float, int, int, BF, BF>(o[q][c], B, A[q]); }); }); }
    const float* MU = P.gf + (size_t)(4 + kvh) * 1280 + 512 + c0 * 16; const bool vpk = P.vpk;
    unroll<VQ>([&](auto qi) { constexpr int q = decltype(qi)::value; if (q < nq) { unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value;
      const int u = (qs0 + q) * 8 + m, row = u / 6; if (row < P.M) { const size_t oi = ((size_t)row * P.NP + s) * A_NH + kvh * 6 + u % 6; const float l = P.pl[oi]; float* po = P.pacc + oi * 256 + c0 * 16;
        unroll<VC>([&](auto ci) { constexpr int c = decltype(ci)::value; vf v = o[q][c].template select<16, 1>(16 * m); if (vpk) v = v + block_load<float, 16>(MU + 16 * c) * l;
          block_store<float, 16>(po + 16 * c, v); }); } }); } }); });
}
// 5. combine the splits, inverse rotation, gate, quantised input of o_proj and its GEMM scales (one work-group per row)
inline void combine(sycl::queue& Q, const Args P) {
  Q.parallel_for(sycl::nd_range<1>(P.M * 12, 12), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 12, blk = gid % 12; vf x[32];
    const int ln = P.pos0 + tok + 1, S = P.mir ? (ln - 1) / P.PS + 1 : ln < A_LONG ? A_SHORT : A_SPL, SP = P.mir ? P.NP : A_SPL;
    unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value; vf* O = x + 16 * hh; const int hd = 2 * blk + hh; const size_t b0 = (size_t)tok * SP * A_NH + hd;
      float M2 = -1e30f; for (int s = 0; s < S; ++s) { const float v = P.pm[b0 + (size_t)s * A_NH]; M2 = M2 > v ? M2 : v; }
      float L = 0.f; unroll<16>([&](auto ki) { O[decltype(ki)::value] = 0.f; });
      for (int s = 0; s < S; ++s) { const size_t i = b0 + (size_t)s * A_NH; const float pv = P.pm[i]; if (!(pv > -1e29f)) continue;       // weight 0: adds +0
        const vf wv = v_expf(vf(pv - M2)); const float w = wv[0]; L = L + w * P.pl[i]; const float* pa = P.pacc + i * 256;
        unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; O[k] = O[k] + block_load<float, 16>(pa + 16 * k) * w; }); }
      const vf Lv(L); unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value; O[k] = vdiv(O[k], Lv); });
      v_fwht256(O);
      const size_t e0 = (size_t)(tok * A_NH + hd) * 256;
      unroll<16>([&](auto ki) { constexpr int k = decltype(ki)::value;
        const vf y = v_rbf((O[k] * 0.0625f) * block_load<float, 16>(P.sg + 16 * k)), y2 = v_rbf(y * v_rbf(v_sigmoid(block_load<float, 16>(P.gate + e0 + 16 * k))));
        block_store<float, 16>(P.y + e0 + 16 * k, y); block_store<float, 16>(P.y2 + e0 + 16 * k, y2); O[k] = y2 * block_load<float, 16>(P.si + hd * 256 + 16 * k); }); });
    const float als = v_a8_tail(x, P.qx, P.alpha, P.MR, tok, blk, 12);
    v_scales((uint32_t)it.get_local_id(0), 12, tok, P.M, P.MR, als, P.us, P.Aw); });
}
}  // namespace ak

// Chain pass of M rows (M <= MRMAX) at cache slots pos0 .. pos0+M-1.  In: E.qx / E.alpha / E.S from res_norm.
// Out: A.o.part / E.us of o_proj, ready for E.res_norm(&A.o, M, ln2, gate.sg_in, false).
inline void attn_forward(Engine& E, Attn& A, int M, int pos0) {
  AttnShared& W = *A.sh; sycl::queue& Q = E.q; const int MR = Engine::mr_of(M), max_ctx = A.max_ctx; const float e = E.eps; const bool vpk = A.vpk;
  if (M < 1 || M > E.cap || pos0 < 0 || pos0 + M > max_ctx) throw std::runtime_error("attn_forward: rows outside the cache");
  auto stamp = [&](int i) { if (attn_prof) { Q.wait(); attn_prof[i] = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); } };
  stamp(0);
  E.gemm_group({&A.q, &A.k, &A.v}, M); stamp(1); E.mark("attn: GEMM q+k+v");
  float *q16 = W.q16, *gate = W.gate, *enc_r = W.enc_r, *enc_c = W.enc_c, *enc_e = W.enc_e, *pm = W.pm, *pl = W.pl, *pacc = W.pacc; uint8_t* enc_st = W.enc_st;
  const float *rope = W.rope, *sg = W.sg, *mult = W.mult, *gf = A.gf; const int *gci = A.gci, *gmeta = A.gmeta;
  uint32_t *kp = A.kp, *vp = A.vp; float *kps = A.kps, *vps = A.vps, *vs = A.vs; int8_t* v8 = A.v8;
  uint16_t *mkp = A.mk, *mvp = A.mv; const int Gn = W.G; const bool mir = W.mir && E.fast;

  ak::Args P{}; const bool fast = E.fast;
  if (fast) { P.aq = A.q.part; P.ak = A.k.part; P.av = A.v.part; P.tq = A.q.tau; P.tk = A.k.tau; P.tv = A.v.tau; P.sq = A.q.sg_out; P.sk = A.k.sg_out; P.sv = A.v.sg_out;
    P.us_in = E.us; P.S = E.S; P.qn = A.qn; P.kn = A.kn; P.rope = rope; P.sg = sg; P.mult = mult; P.gf = gf; P.si = A.o.sg_in; P.gci = gci; P.gmeta = gmeta;
    P.q16 = q16; P.gate = gate; P.y = W.y; P.y2 = W.y2; P.enc_r = enc_r; P.enc_c = enc_c; P.enc_e = enc_e; P.pm = pm; P.pl = pl; P.pacc = pacc; P.kps = kps; P.vps = vps; P.vs = vs;
    P.us = E.us; P.alpha = E.alpha; P.enc_st = enc_st; P.vbp = W.vbp; P.vxi = W.vxi; P.kp = kp; P.vp = vp; P.v8 = v8; P.qx = E.qx; P.Aw = E.Aw;
    P.M = M; P.MR = MR; P.pos0 = pos0; P.max_ctx = max_ctx; P.eps = e; P.vpk = vpk;
    P.mk = A.mk; P.mv = A.mv; P.sc = W.sc; P.pa = W.pa; P.qa = W.qa; P.G = W.G; P.NP = W.NP; P.PS = A_PS; P.NQG = W.NQG; P.mir = W.mir; { static const int dbg = std::getenv("YX_ADBG") ? atoi(std::getenv("YX_ADBG")) : 0; P.dbg = dbg; }      // tests: 1 = score without the products, 2 = without the softmax sweep, 4 = values without the products (wrong results)
    if (W.mir) { P.pm = W.pm2; P.pl = W.pl2; P.pacc = W.pacc2; } }
  // 1. post: projection outputs -> q16, gate, rotated K / V rows -> INT8 row -> encoder residual
  if (fast) ak::post(Q, P); else { const int64_t *aq = A.q.part, *ak = A.k.part, *av = A.v.part; const float *tq = A.q.tau, *tk = A.k.tau, *tv = A.v.tau, *sq = A.q.sg_out, *sk = A.k.sg_out, *sv = A.v.sg_out;
    const float *usp = E.us, *SS = E.S, *qn = A.qn, *kn = A.kn;
    Q.parallel_for(sycl::range<1>(M * 28), [=](sycl::id<1> it) { const int tok = it[0] / 28, blk = it[0] % 28, pos = pos0 + tok;
      float buf[512]; const float r = rfac(SS + tok * SST, e), u = usp[tok];
      const int64_t* acc = blk < 24 ? aq + (size_t)tok * A_NQ + blk * 512 : blk < 26 ? ak + (size_t)tok * A_NKV + (blk - 24) * 512 : av + (size_t)tok * A_NKV + (blk - 26) * 512;
      const float* tau = blk < 24 ? tq + blk * 512 : blk < 26 ? tk + (blk - 24) * 512 : tv + (blk - 26) * 512;
      const float* so = blk < 24 ? sq + blk * 512 : blk < 26 ? sk + (blk - 24) * 512 : sv + (blk - 26) * 512;
      for (int i = 0; i < 512; ++i) buf[i] = yrot_n(acc[i], u, r, tau[i]);
      fwht512(buf);
      for (int i = 0; i < 512; ++i) buf[i] = rbf((buf[i] * K512) * so[i]);
      const float* cs = rope + (size_t)pos * 64;
      if (blk < 24) {
        float* go = gate + (size_t)(tok * 24 + blk) * 256; for (int d = 0; d < 256; ++d) go[d] = buf[256 + d];
        norm_rope(buf, qn, cs, e); kv_rot(buf, sg, false);
        float* qo = q16 + (size_t)(tok * 24 + blk) * 256; for (int d = 0; d < 256; ++d) qo[d] = rbf(buf[d]);
        return;
      }
      const int kind = blk >= 26;
      for (int hh = 0; hh < 2; ++hh) { float* x = buf + hh * 256; const int g8 = kind * 4 + (blk - 24 - 2 * kind) * 2 + hh;
        if (!kind) norm_rope(x, kn, cs, e);
        kv_rot(x, sg, false);
        float t[256]; const float sc = kv_store8f(x, t); for (int d = 0; d < 256; ++d) x[d] = t[d] * sc;      // value of the INT8 row
        float* er = enc_r + (size_t)(tok * 8 + g8) * 256; float* ec = enc_c + (tok * 8 + g8) * 4; const int* mt = gmeta + g8 * 4; const float* G = gf + (size_t)g8 * 1280;
        if (pos < mt[0]) { for (int d = 0; d < 256; ++d) er[d] = x[d]; ec[3] = 1.f; continue; }                 // guard row: no trellis
        if (mt[2]) for (int d = 0; d < 256; ++d) x[d] = x[d] - G[512 + d];
        ec[0] = 0.f; ec[1] = 0.f;
        for (int j = 0; j < mt[1]; ++j) { const float* U = G + (3 + j) * 256; for (int d = 0; d < 256; ++d) t[d] = x[d] * U[d];
          const float c = rf16(s256(t)); ec[j] = c; for (int d = 0; d < 256; ++d) x[d] = x[d] - c * U[d]; }
        for (int d = 0; d < 256; ++d) t[d] = x[d] * x[d];
        float rms = fsqrt(fdiv(s256(t), 256.f)); if (rms == 0.f) rms = 1.f;
        ec[2] = rms; ec[3] = 0.f; for (int d = 0; d < 256; ++d) er[d] = x[d]; } }); }
  stamp(2);

  // 2. trellis paths: one work-item per (row, group, scale, chain of 64 values)
  if (fast) ak::viterbi(Q, P); else
  Q.parallel_for(sycl::range<1>(M * 8 * A_NM * 4), [=](sycl::id<1> it) { const int ch = it[0] & 3, mi = (it[0] >> 2) % A_NM, tg = it[0] / (4 * A_NM), g8 = tg & 7;
    const float* ec = enc_c + tg * 4; if (ec[3] != 0.f) return;
    const float* r = enc_r + (size_t)tg * 256 + ch * 64; const int* ci = gci + g8 * 256; const float* cdec = gf + (size_t)g8 * 1280;
    float s = rf16(ec[2] * mult[mi]); if (s == 0.f) s = 1.f;
    int cost[256]; uint8_t bp[64 * 8];
    for (int i = 0; i < 256; ++i) cost[i] = 0x40000000; cost[0] = 0;
    for (int t = 0; t < 64; ++t) {
      float xf = sycl::rint(fdiv(r[t], s) * 256.f); xf = xf < -2048.f ? -2048.f : (xf > 2048.f ? 2048.f : xf); const int xi = (int)xf;
      int mn[8];
      for (int uu = 0; uu < 8; ++uu) { int best = cost[uu], jb = 0; for (int j = 1; j < 32; ++j) { const int c = cost[j * 8 + uu]; if (c < best) { best = c; jb = j; } } mn[uu] = best; bp[t * 8 + uu] = (uint8_t)jb; }
      for (int st = 0; st < 256; ++st) { const int d = xi - ci[st]; cost[st] = d * d + mn[st >> 5]; }
    }
    int stt = 0, best = cost[0]; for (int i = 1; i < 256; ++i) if (cost[i] < best) { best = cost[i]; stt = i; }
    uint8_t* so = enc_st + ((size_t)tg * A_NM + mi) * 256 + ch * 64; float dd[64];
    for (int t = 63; t >= 0; --t) { so[t] = (uint8_t)stt; const float d = r[t] - cdec[stt] * s; dd[t] = d * d; stt = ((int)bp[t * 8 + (stt >> 5)] << 3) | (stt >> 5); }
    float* eo = enc_e + ((size_t)tg * A_NM + mi) * 8 + ch * 2; eo[0] = xor_tree32(dd); eo[1] = xor_tree32(dd + 32); });
  stamp(3); E.mark("attn: post + trellis encode");

  // 3. select the best scale; pack (or store8 of the reconstruction for an unpacked V stream) into the cache
  Q.parallel_for(sycl::range<1>(M * 8), [=](sycl::id<1> it) { const int tg = it[0], tok = tg >> 3, g8 = tg & 7, kind = g8 >> 2, kvh = g8 & 3;
    const size_t row = (size_t)kvh * max_ctx + pos0 + tok; const float* ec = enc_c + tg * 4; const float* r = enc_r + (size_t)tg * 256;
    const int* mt = gmeta + g8 * 4; const float* G = gf + (size_t)g8 * 1280; float x[256];
    int mi = 0; float s = 0.f; const uint8_t* st = enc_st + (size_t)tg * A_NM * 256;
    if (ec[3] != 0.f) { for (int d = 0; d < 256; ++d) x[d] = r[d]; }
    else {
      float eb = 0.f;
      for (int m = 0; m < A_NM; ++m) { const float* ee = enc_e + ((size_t)tg * A_NM + m) * 8; float t = 0.f; for (int w = 0; w < 8; ++w) t = t + ee[w]; if (m == 0 || t < eb) { eb = t; mi = m; } }
      st += (size_t)mi * 256; s = rf16(ec[2] * mult[mi]); if (s == 0.f) s = 1.f;
      if (!kind || vpk) {
        uint32_t w[40]; for (int i = 0; i < 40; ++i) w[i] = 0;
        for (int el = 0; el < 256; ++el) { const uint32_t f = st[el] & 31u; const int g = 5 * el, wi = g >> 5, off = g & 31;
          if (off <= 27) w[wi] |= f << (27 - off); else { w[wi] |= f >> (off - 27); w[wi + 1] |= f << (59 - off); } }
        uint32_t* wo = (kind ? vp : kp) + row * 40; for (int i = 0; i < 40; ++i) wo[i] = w[i];
        (kind ? vps : kps)[row] = s;
        if (mir) { const int p = pos0 + tok, jl = p & 15; uint16_t* mo = (kind ? mvp : mkp) + ((size_t)kvh * Gn + (p >> 4)) * 4096; const float* T = G + 256; uint32_t stt = 0;
          for (int el = 0; el < 256; ++el) { if ((el & 63) == 0) stt = 0; stt = ((stt << 5) | (st[el] & 31u)) & 255u;
            const uint16_t hb = (uint16_t)(sycl::bit_cast<uint32_t>(T[stt]) >> 16); const int c = el >> 4, dl = el & 15;
            if (!kind) mo[((c * 8 + (dl >> 1)) * 16 + jl) * 2 + (dl & 1)] = hb; else mo[((c * 8 + (jl >> 1)) * 16 + dl) * 2 + (jl & 1)] = hb; } }
        return;
      }
      for (int d = 0; d < 256; ++d) x[d] = G[st[d]] * s;
      for (int j = 0; j < mt[1]; ++j) { const float* U = G + (3 + j) * 256; const float c = ec[j]; for (int d = 0; d < 256; ++d) x[d] = x[d] + c * U[d]; }
      if (mt[2]) for (int d = 0; d < 256; ++d) x[d] = x[d] + G[512 + d];
    }
    float t[256]; const float sc = kv_store8f(x, t); int8_t* vo = v8 + row * 256; for (int d = 0; d < 256; ++d) vo[d] = (int8_t)(int)t[d];
    vs[row] = sc;
    if (mir) { const int p = pos0 + tok, jl = p & 15; uint16_t* mo = mvp + ((size_t)kvh * Gn + (p >> 4)) * 4096;
      for (int d = 0; d < 256; ++d) mo[(((d >> 4) * 8 + (jl >> 1)) * 16 + (d & 15)) * 2 + (jl & 1)] = (uint16_t)(sycl::bit_cast<uint32_t>(t[d]) >> 16); } });
  stamp(4); E.mark("attn: select + mirror write");

  // 4. partial softmax of one (row, query head, split)
  // attention kernels: 0 default (score16 + vacc16, the reference); experiments (not bit-identical to 0): 1 fa48, 2 pipeline 48 (RG 2),
  // 3 FA24 (RG 2, phases of fa48), 4 pipeline 24 (RG 1), 5 pipeline 48 + vector rescale, 6 fa48 phases + vector rescale (RG 4),
  // 7 separate kernels with the online softmax, 8 pipeline 24 (RG 2), 9 pipeline 48 (RG 1), 10 FA24 (RG 4), 11 pipeline 24 (RG 2) + vector rescale
  const int fa = g_attn_mode;
  if (fast && W.mir && fa) { E.mark(nullptr);
    switch (fa) { case 1: ak::fa48(Q, P); break; case 2: ak::fa_p<16, 2, 1, 0>(Q, P); break; case 3: ak::fa_p<8, 2, 0, 0>(Q, P); break; case 4: ak::fa_p<8, 1, 1, 0>(Q, P); break;
      case 5: ak::fa_p<16, 2, 1, 1>(Q, P); break; case 6: ak::fa_p<16, 4, 0, 1>(Q, P); break; case 7: ak::score16o(Q, P); ak::vacc16o(Q, P); break; case 8: ak::fa_p<8, 2, 1, 0>(Q, P); break;
      case 9: ak::fa_p<16, 1, 1, 0>(Q, P); break; case 10: ak::fa_p<8, 4, 0, 0>(Q, P); break; case 11: ak::fa_p<8, 2, 1, 1>(Q, P); break;
      default: throw std::runtime_error("YX_ATTN_FA: unknown attention experiment"); }
    E.mark("attn: fused scores + values (experiment)"); }
  else if (fast && W.mir) { static const int cfg = std::getenv("YX_SV") ? atoi(std::getenv("YX_SV")) : 550; const int qs = cfg / 100, vq = cfg / 10 % 10, vc = cfg % 10;  // digits: score kernel (5 = work-group), value kernel (5 = work-group) and its dim chunks
    if (qs == 5) ak::score16(Q, P); else if (qs == 3) ak::score1(Q, P); else if (qs == 1) ak::score<1>(Q, P); else if (qs == 2) ak::score<2>(Q, P); else ak::score<4>(Q, P);
    E.mark("attn: scores (K load, QxK, softmax)");
    if (vq == 5) ak::vacc16(Q, P); else if (vq == 1 && vc == 4) ak::vacc<1, 4>(Q, P); else if (vq == 1) ak::vacc<1, 8>(Q, P); else if (vq == 2 && vc == 2) ak::vacc<2, 2>(Q, P); else if (vq == 2) ak::vacc<2, 4>(Q, P); else ak::vacc<4, 2>(Q, P); }
  else if (fast) { static const int ng = std::getenv("YX_ATTN_NG") ? atoi(std::getenv("YX_ATTN_NG")) : 0; const int NG = ng ? ng : (M <= 2 ? 1 : 3);     // query heads per thread (measured: 1 is best for M = 1, 3 for M = 8)
    if (NG == 1) ak::attend<1>(Q, P); else if (NG == 2) ak::attend<2>(Q, P); else if (NG == 3) ak::attend<3>(Q, P); else ak::attend<6>(Q, P); }
  else
  Q.parallel_for(sycl::range<1>((size_t)M * A_NH * A_SPL), [=](sycl::id<1> it) { const int s = it[0] % A_SPL, th = it[0] / A_SPL, hd = th % A_NH, tok = th / A_NH, kvh = hd / A_GQ;
    const int ln = pos0 + tok + 1, own = pos0 + tok, S = ln < A_LONG ? A_SHORT : A_SPL; if (s >= S) return;
    const int per = (ln + S - 1) / S, lo = s * per, hi = ln < lo + per ? ln : lo + per, phh = hi < ln - 1 ? hi : ln - 1, ph = lo > phh ? lo : phh;
    const bool has_self = lo <= ln - 1 && ln - 1 < lo + per; const size_t oi = ((size_t)tok * A_SPL + s) * A_NH + hd;
    if (!(ph > lo || has_self)) { pm[oi] = -1e30f; pl[oi] = 0.f; return; }
    const float* qv = q16 + (size_t)(tok * A_NH + hd) * 256; const float* tabK = gf + (size_t)kvh * 1280 + 256; const float* GV = gf + (size_t)(4 + kvh) * 1280; const float* tabV = GV + 256;
    const size_t r0 = (size_t)kvh * max_ctx; const float* vsc = vpk ? vps : vs;
    float m = -1e30f, l = 0.f; float o[256]; for (int d = 0; d < 256; ++d) o[d] = 0.f;
    const float NINF = -std::numeric_limits<float>::infinity();
    for (int t0 = (lo / 32) * 32; t0 < ph; t0 += 32) { const int a = t0 > lo ? t0 : lo, b = t0 + 32 < ph ? t0 + 32 : ph; if (b <= a) continue;
      float sc[32], p[32]; for (int i = 0; i < 32; ++i) sc[i] = NINF;
      float mx = NINF;
      for (int j = a; j < b; ++j) { const uint32_t* w = kp + (r0 + j) * 40; float acc = 0.f;        // prefix-key dot: 16-element chunks in order, XOR tree inside
        for (int c = 0; c < 4; ++c) { uint32_t st = 0;
          for (int cc = 0; cc < 4; ++cc) { float pr[16];
            for (int i = 0; i < 16; ++i) { const int el = cc * 16 + i; st = ((st << 5) | tb_field(w + c * 10, el)) & 255u; pr[i] = qv[c * 64 + el] * tabK[st]; }
            acc = acc + tree16(pr); } }
        const float v = acc * (kps[r0 + j] * 0.0625f); sc[j - t0] = v; mx = sycl::fmax(mx, v); }
      const float x = sycl::fmax(m, mx), c = pm_exp2f((m - x) * A_LOG2E), y0 = x * A_LOG2E; m = x;
      for (int i = 0; i < 32; ++i) p[i] = (i >= a - t0 && i < b - t0) ? pm_exp2f(sc[i] * A_LOG2E - y0) : 0.f;
      float t[4];
      for (int rr = 0; rr < 4; ++rr) t[rr] = (((p[2 * rr] + p[2 * rr + 1]) + (p[8 + 2 * rr] + p[8 + 2 * rr + 1])) + (p[16 + 2 * rr] + p[16 + 2 * rr + 1])) + (p[24 + 2 * rr] + p[24 + 2 * rr + 1]);
      l = l * c + ((t[0] + t[1]) + (t[2] + t[3]));
      for (int d = 0; d < 256; ++d) o[d] = o[d] * c;
      for (int j = a; j < b; ++j) { const float pf = rbf(p[j - t0] * vsc[r0 + j]);                    // BF16 rounding of p*vs; keys in slot order
        if (vpk) { const uint32_t* w = vp + (r0 + j) * 40;
          for (int cq = 0; cq < 4; ++cq) { uint32_t st = 0; for (int el = 0; el < 64; ++el) { st = ((st << 5) | tb_field(w + cq * 10, el)) & 255u; const int d = cq * 64 + el; o[d] = o[d] + pf * tabV[st]; } } }
        else { const int8_t* vv = v8 + (r0 + j) * 256; for (int d = 0; d < 256; ++d) o[d] = o[d] + pf * (float)vv[d]; } }
    }
    if (has_self) { const uint32_t* w = kp + (r0 + own) * 40; float p4[4] = {0.f, 0.f, 0.f, 0.f}; float kv[16];
      for (int c = 0; c < 4; ++c) { uint32_t st = 0;
        for (int cc = 0; cc < 4; ++cc) { const int e0 = c * 64 + cc * 16;
          for (int i = 0; i < 16; ++i) { st = ((st << 5) | tb_field(w + c * 10, cc * 16 + i)) & 255u; kv[i] = qv[e0 + i] * tabK[st]; }
          for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) for (int rr = 0; rr < 4; ++rr) p4[rr] = p4[rr] + kv[8 * a + 2 * rr + b]; } }
      const float sc = ((p4[0] + p4[1]) + (p4[2] + p4[3])) * (kps[r0 + own] * 0.0625f);
      const float mn = sycl::fmax(m, sc), c = pm_exp2f((m - mn) * A_LOG2E), p = pm_exp2f((sc - mn) * A_LOG2E); m = mn; l = l * c + p;
      const float pv = p * vsc[r0 + own];
      if (vpk) { const uint32_t* wv = vp + (r0 + own) * 40;
        for (int cq = 0; cq < 4; ++cq) { uint32_t st = 0; for (int el = 0; el < 64; ++el) { st = ((st << 5) | tb_field(wv + cq * 10, el)) & 255u; const int d = cq * 64 + el; o[d] = o[d] * c + pv * tabV[st]; } } }
      else { const int8_t* vv = v8 + (r0 + own) * 256; for (int d = 0; d < 256; ++d) o[d] = o[d] * c + pv * (float)vv[d]; } }
    if (vpk) { const float* mu = GV + 512; for (int d = 0; d < 256; ++d) o[d] = o[d] + l * mu[d]; }
    pm[oi] = m; pl[oi] = l; float* po = pacc + oi * 256; for (int d = 0; d < 256; ++d) po[d] = o[d]; });
  stamp(5); E.mark("attn: values (V load, PxV, write)");

  // 5. combine the splits, inverse rotation, gate, quantised input of o_proj
  if (fast) { ak::combine(Q, P); E.aw_ready = true; } else { float *yo = W.y, *y2o = W.y2, *al = E.alpha; int8_t* qq = E.qx; const float* si = A.o.sg_in;
    Q.parallel_for(sycl::range<1>(M * 12), [=](sycl::id<1> it) { const int tok = it[0] / 12, blk = it[0] % 12; float buf[512];
      const int ln = pos0 + tok + 1, S = ln < A_LONG ? A_SHORT : A_SPL;
      for (int hh = 0; hh < 2; ++hh) { const int hd = 2 * blk + hh; float* O = buf + hh * 256; const size_t b0 = (size_t)tok * A_SPL * A_NH + hd;
        float M2 = -1e30f; for (int s = 0; s < S; ++s) M2 = sycl::fmax(M2, pm[b0 + (size_t)s * A_NH]);
        float L = 0.f; for (int d = 0; d < 256; ++d) O[d] = 0.f;
        for (int s = 0; s < S; ++s) { const size_t i = b0 + (size_t)s * A_NH; const float pv = pm[i]; if (!(pv > -1e29f)) continue;       // weight 0: adds +0
          const float w = pm_expf(pv - M2); L = L + w * pl[i]; const float* pa = pacc + i * 256; for (int d = 0; d < 256; ++d) O[d] = O[d] + w * pa[d]; }
        for (int d = 0; d < 256; ++d) O[d] = fdiv(O[d], L);
        kv_rot(O, sg, true);
        const size_t e0 = (size_t)(tok * A_NH + hd) * 256;
        for (int d = 0; d < 256; ++d) { const float y = rbf(O[d]), y2 = rbf(y * rbf(pm_sigmoid(gate[e0 + d]))); yo[e0 + d] = y; y2o[e0 + d] = y2; O[d] = y2 * si[hd * 256 + d]; } }
      a8_tail(buf, qq, al, MR, tok, blk, 12); }); }
  E.mark("attn: combine"); E.gemm_group({&A.o}, M); stamp(6); E.mark("attn: GEMM o");
}
}  // namespace yx
