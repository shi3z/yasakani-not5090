// SPDX-License-Identifier: Apache-2.0
// YASAKANI default path for Intel Xe2 in SYCL.  Math: spec/A_gemv.md, spec/B_layers.md (pm math).
#pragma once
#include "gemm.hpp"
#include "kern.hpp"
#include "fast.hpp"
#include "model.hpp"
#include <chrono>
#include <filesystem>
#include <map>
namespace yx {
constexpr int MRMAX = 16;      // rows of a decode / verify pass
// Prefill batch (rows of a committing pass): YX_PF_BATCH = 16, 32, 64, ... (scratch buffers are sized for it)
inline int pf_batch() { static const int v = [] { const char* e = std::getenv("YX_PF_BATCH"); int b = e ? atoi(e) : 256; return b <= 16 ? 16 : b <= 64 ? 64 : b <= 128 ? 128 : 256; }(); return v; }
constexpr int NL = 64, F = 17408, ZN = 6144, CV = 10240, VOC = 248320;
struct Gdn { Tensor qkv, z, out; float *wa, *wb, *conv, *A_log, *dt_bias, *gnorm; float* S; float* hist;        // S [48][128][128], hist [3][10240]
             uint16_t* mixed; float *q, *k, *v, *beta, *g; };                                                  // per-layer verify intermediates
struct Mlp { Tensor gate, up, down; };
struct Layer { bool full = false; Gdn g; Mlp m; float *ln1, *ln2; };
struct Engine {
  sycl::queue q; Container C; float eps = 1e-6f; int nlayers = 0; std::string model_path;
  std::vector<Layer> L; Tensor head; float* fnorm = nullptr;
  uint32_t* emb_codes = nullptr; float* emb_lut = nullptr; float* emb_sc = nullptr; float* emb_sg = nullptr;
  // per-pass scratch
  float *h, *S, *us, *alpha, *z, *ab, *core, *logits; int8_t* qx; int32_t* Aw; uint16_t* xn; int* ids; int64_t* part;
  // Fast path (default): ESIMD kernels of fast.hpp, bit-identical to the scalar kernels below (tests/fast_cmp.cpp).  YX_SCALAR=1 selects the scalar kernels.
  // aw_ready: the fast a8 producers (res_norm, the GDN out stage, the MLP stage) also derive us / Aw of the next GEMM from alpha, so that
  // gemm_group can skip its "prep" kernel.  gemm_group clears the flag; a producer that writes alpha / qx by other means needs nothing
  // as long as the previous fast producer was consumed by a gemm_group (otherwise set aw_ready = false).
  int cap = 16;                   // row capacity of the scratch buffers (>= MRMAX) = the prefill batch, chosen in the constructor
  const bool fast = std::getenv("YX_SCALAR") == nullptr; bool aw_ready = false; float* Sscr = nullptr; int64_t* partw = nullptr; const bool wsplit = std::getenv("YX_NOSPLIT") == nullptr;   // Sscr: state copy for the rows after n_commit
  // stage profile (tests): when prof is set, mark(name) waits for the queue and adds the time since the previous mark to T[name]
  bool prof = false; std::map<std::string, double> T; double tl = 0;
  void mark(const char* n) { if (!prof) return; q.wait(); const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); if (n) T[n] += t - tl; tl = t; }
  template <class T> T* dev(size_t n) { T* p = (T*)dev_alloc(q, n * sizeof(T)); if (p) q.memset(p, 0, n * sizeof(T)); return p; }
  Tensor up(const std::string& n) { Tensor T = upload(q, C, n); T.part = part; return T; }

  double mem_need = 0, mem_avail = 0;     // set by the API: estimate for the chosen batch / free device memory at start (bytes)
  // cap_arg: rows of a prefill batch (16 / 64 / 128 / 256); 0 = YX_PF_BATCH or 256 (tests)
  explicit Engine(const std::string& path, int upto = NL, int cap_arg = 0) : q(sycl::gpu_selector_v, sycl::property::queue::in_order()) {
    C.open(path); model_path = path; cap = cap_arg > 0 ? (cap_arg <= 16 ? 16 : cap_arg <= 64 ? 64 : cap_arg <= 128 ? 128 : 256) : pf_batch(); nlayers = upto; eps = C.hdr["model"]["eps"].get<float>();
    if (!g_dry && fast && fx::esimd_contracts(q)) throw std::runtime_error("ESIMD kernels contract a*b+c: build with -Xsycl-target-backend \"-ffp-contract=off\"");
    Sscr = dev<float>(48 * 128 * 128); partw = dev<int64_t>((size_t)10 * 16 * 2 * F);
    part = dev<int64_t>((size_t)cap * 2 * F);      // the largest launch group: gate + up
    h = dev<float>((size_t)cap * H); S = dev<float>(cap * SST); us = dev<float>(cap); alpha = dev<float>(cap * 34);
    qx = dev<int8_t>((size_t)cap * F); Aw = dev<int32_t>(cap * 34); xn = dev<uint16_t>((size_t)cap * H); ids = dev<int>(cap);
    z = dev<float>((size_t)cap * ZN); ab = dev<float>(cap * 96); core = dev<float>((size_t)cap * ZN);
    { const json& t = C.tensor("embed_tokens"); const int bits = t["bits"];
      emb_codes = to_dev(q, (const uint32_t*)(C.base + t["stream_off"].get<uint64_t>()), (size_t)VOC * 20 * 4 * bits);
      const uint16_t* l16 = (const uint16_t*)(C.base + t["lut_off"].get<uint64_t>()); std::vector<float> lf(8192); for (int i = 0; i < 8192; ++i) lf[i] = h2f_host(l16[i]);
      emb_lut = to_dev(q, lf.data(), 8192);
      const uint16_t* s16 = (const uint16_t*)(C.base + t["scale_off"].get<uint64_t>()); std::vector<float> sf(VOC); for (int i = 0; i < VOC; ++i) sf[i] = h2f_host(s16[i]);
      emb_sc = to_dev(q, sf.data(), VOC);
      auto s = sign_stream(t["rot_in"]["sign_seed"].get<uint64_t>(), t["rot_in"]["sign_offset"].get<uint64_t>(), H); std::vector<float> f(s.begin(), s.end()); emb_sg = to_dev(q, f.data(), H); }
    L.resize(nlayers + 1);
    for (int l = 0; l <= nlayers && l < NL; ++l) {
      Layer& Y = L[l]; const std::string p = "layers." + std::to_string(l) + "."; Y.full = (l % 4 == 3);
      Y.ln1 = upload_vec(q, C, p + "input_layernorm", H);
      if (l == nlayers) break;
      Y.ln2 = upload_vec(q, C, p + "post_attention_layernorm", H);
      if (!Y.full) { Gdn& g = Y.g; const std::string a = p + "linear_attn.";
        g.qkv = up(a + "in_proj_qkv"); g.z = up(a + "in_proj_z"); g.out = up(a + "out_proj"); g.z.part = part + (size_t)cap * CV;
        g.wa = upload_vec(q, C, a + "in_proj_a", 48 * H); g.wb = upload_vec(q, C, a + "in_proj_b", 48 * H); g.conv = upload_vec(q, C, a + "conv1d", CV * 4);
        g.A_log = upload_vec(q, C, a + "A_log", 48); g.dt_bias = upload_vec(q, C, a + "dt_bias", 48); g.gnorm = upload_vec(q, C, a + "norm", 128);
        g.S = dev<float>(48 * 128 * 128); g.hist = dev<float>(3 * CV); g.mixed = dev<uint16_t>((size_t)cap * CV);
        g.q = dev<float>(cap * 2048); g.k = dev<float>(cap * 2048); g.v = dev<float>(cap * ZN); g.beta = dev<float>(cap * 48); g.g = dev<float>(cap * 48); }
      Mlp& m = Y.m; m.gate = up(p + "mlp.gate_proj"); m.up = up(p + "mlp.up_proj"); m.down = up(p + "mlp.down_proj"); m.up.part = part + (size_t)cap * F;
    }
    if (!g_dry) q.wait();
  }
  static int mr_of(int M) { return M <= 1 ? 1 : M <= 2 ? 2 : M <= 4 ? 4 : M <= 8 ? 8 : M <= 16 ? 16 : M <= 64 ? 64 : M <= 128 ? 128 : 256; }     // row stride of qx / Aw

  // ---- GEMM launch: scales (us, Aw) from alpha, then one kernel per matrix ----
  void gemm_group(std::initializer_list<Tensor*> ts, int M) {
    const int MR = mr_of(M); Tensor* T0 = *ts.begin(); const int G = T0->G; float* al = alpha; float* usp = us; int32_t* aw = Aw;
    if (!(fast && aw_ready))
    q.parallel_for(sycl::range<1>(MR), [=](sycl::id<1> id) { const int tok = id[0];
      if (tok >= M) { for (int g = 0; g < G; ++g) aw[tok * G + g] = 0; return; }
      float m = 0.f; for (int g = 0; g < G; ++g) m = sycl::fmax(m, al[tok * G + g]);
      const float rm = m > 0.f ? fdiv(16777216.f, m) : 0.f; usp[tok] = m * 5.9604644775390625e-08f;
      for (int g = 0; g < G; ++g) aw[tok * G + g] = (int32_t)sycl::rint(al[tok * G + g] * rm); });
    aw_ready = false;
    if (getenv("YX_SYNC")) q.wait();
    if (getenv("YX_GEMM1")) {       // one launch per matrix (the original path)
    for (Tensor* T : ts) {
#define YX_G(B, R) if (T->bits == B && MR == R) gemm<B, R>(q, T->stream, T->lut, qx, Aw, T->part, T->N, T->Kc);
      YX_G(6, 1) YX_G(6, 2) YX_G(6, 4) YX_G(6, 8) YX_G(6, 16) YX_G(8, 1) YX_G(8, 2) YX_G(8, 4) YX_G(8, 8) YX_G(8, 16)
#undef YX_G
    }
    return; }
    // matrices of the group that differ in code width or value table (quality model: 3- and 4-bit matrices) go to separate launches
    { bool same = true; for (Tensor* T : ts) same = same && T->bits == T0->bits && T->lut_id == T0->lut_id;
      if (!same) { std::vector<Tensor*> rest(ts.begin(), ts.end());
        while (!rest.empty()) { Tensor* a = rest[0]; std::vector<Tensor*> grp, other; for (Tensor* T : rest) (T->bits == a->bits && T->lut_id == a->lut_id ? grp : other).push_back(T);
          aw_ready = true;          // us / Aw are already valid (prepared above or by the producer)
          if (grp.size() == 1) gemm_group({grp[0]}, M); else if (grp.size() == 2) gemm_group({grp[0], grp[1]}, M); else gemm_group({grp[0], grp[1], grp[2]}, M);
          rest = other; }
        return; } }
    // one launch for the whole group; a lone matrix with few rows is split over W workers (partials summed below)
    GemmT gt[3]; int nt = 0, thr = 0; for (Tensor* T : ts) { gt[nt++] = GemmT{T->stream, T->part, T->N}; thr += T->N / 16; }
    if (MR >= 128 && !getenv("YX_BIG1")) {     // prefill batch: weights decoded once per launch, 16 threads per 16 output rows, sums in registers (64 rows: the row-set kernel is faster)
      const int bits = T0->bits;
#define YX_O(B, R, D, ND) if (bits == B && MR == R) gemm_once<B, D, ND>(q, gt, nt, T0->lut, qx, Aw, T0->Kc);
      YX_O(6, 64, 4, 1) YX_O(6, 128, 8, 1) YX_O(6, 256, 8, 2) YX_O(8, 64, 4, 1) YX_O(8, 128, 8, 1) YX_O(8, 256, 8, 2)
#undef YX_O
      return; }
    if (MR > 16) {       // prefill batch: TR tiles of 8 rows per thread; fewer tiles (more threads, more decoding) when the launch would leave the GPU idle
      const int trf = getenv("YX_TR") ? atoi(getenv("YX_TR")) : 0; int TR = 4; while (TR > 2 && thr * (MR / (8 * TR)) < 640) TR /= 2; if (trf) TR = trf;      // measured (64 rows, gate+up): 32 rows per thread 0.82 ms, 16: 1.22, 64: 1.16
      if (MR % (8 * TR)) throw std::runtime_error("gemm: bad tile count");
      const int bits = T0->bits;
#define YX_B(B, R) if (bits == B && TR == R) gemm_big<B, R>(q, gt, nt, T0->lut, qx, Aw, T0->Kc, MR);
      YX_B(6, 1) YX_B(6, 2) YX_B(6, 4) YX_B(6, 8) YX_B(8, 1) YX_B(8, 2) YX_B(8, 4) YX_B(8, 8)
#undef YX_B
      return; }
    int W = 1; if (nt == 1 && wsplit && thr < 640) W = 2;      // the GPU runs about 640 ESIMD threads at once; 320 leave half of it idle
    if (getenv("YX_W") && nt == 1 && T0->N == H) W = std::min(G, atoi(getenv("YX_W")));
    if (getenv("YX_WG") && nt > 1) W = std::min(G, atoi(getenv("YX_WG")));
    size_t poff[3] = {0, 0, 0}; if (W > 1) { size_t o = 0; for (int k = 0; k < nt; ++k) { poff[k] = o; gt[k].part = partw + o; o += (size_t)W * MR * gt[k].N; } }
    const int bits = T0->bits;
#define YX_G(B, R) if (bits == B && MR == R) gemm_multi<B, R>(q, gt, nt, T0->lut, qx, Aw, T0->Kc, W);
    YX_G(6, 1) YX_G(6, 2) YX_G(6, 4) YX_G(6, 8) YX_G(6, 16) YX_G(8, 1) YX_G(8, 2) YX_G(8, 4) YX_G(8, 8) YX_G(8, 16)
#undef YX_G
    if (W > 1) { int k = 0; for (Tensor* T : ts) { const int64_t* pw = partw + poff[k++]; int64_t* out = T->part; const int N = T->N; const size_t n = (size_t)MR * N;
      q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { int64_t s = 0; for (int w = 0; w < W; ++w) s += pw[(size_t)w * n + i[0]]; out[i[0]] = s; }); } }
  }
  // ---- embedding rows -> h ----
  void embed(int M) {
    const uint32_t* codes = emb_codes; const float *lut = emb_lut, *sc = emb_sc, *sg = emb_sg; float* hh = h; const int* id = ids;
    if (fast) { fx::k_embed(q, M, codes, lut, sc, sg, hh, id); return; }
    q.parallel_for(sycl::range<1>(M * 10), [=](sycl::id<1> it) { const int tok = it[0] / 10, b = it[0] % 10; const int tid = id[tok]; const float s = sc[tid];
      float x[512];
      for (int hf = 0; hf < 2; ++hf) { const uint32_t* w = codes + (size_t)tid * 480 + (2 * b + hf) * 24;
        for (int j = 0; j < 128; ++j) { const int e = 6 * (j + 1); uint32_t st;
          if (e >= 12) { const int a = e - 12, wi = a >> 5, sh = a & 31; st = w[wi] >> sh; if (sh + 12 > 32) st |= w[wi + 1] << (32 - sh); st &= 0xfffu; }
          else st = (w[0] & ((1u << e) - 1u)) << (12 - e);
          x[hf * 256 + 2 * j] = lut[2 * st] * s; x[hf * 256 + 2 * j + 1] = lut[2 * st + 1] * s; } }
      fwht512(x);
      for (int i = 0; i < 512; ++i) hh[(size_t)tok * H + b * 512 + i] = rbf((x[i] * K512) * sg[b * 512 + i]); });
  }
  // ---- residual add (T may be null) + per-block sums of squares + quantised input of the next GEMM ----
  void res_norm(const Tensor* T, int M, const float* w, const float* sg_in, bool want_xn) {
    const int MR = mr_of(M); float *hh = h, *SS = S, *al = alpha; int8_t* qq = qx; uint16_t* xu = want_xn ? xn : nullptr;
    const int64_t* acc = T ? T->part : nullptr; const float* tau = T ? T->tau : nullptr; const float* so = T ? T->sg_out : nullptr; const float* usp = us;
    if (fast) { fx::k_res_norm(q, M, MR, hh, SS, al, qq, xu, acc, tau, so, us, Aw, w, sg_in); aw_ready = true; mark("res_norm"); return; }
    q.parallel_for(sycl::range<1>(M * 10), [=](sycl::id<1> it) { const int tok = it[0] / 10, blk = it[0] % 10; float buf[512]; float* hp = hh + (size_t)tok * H + blk * 512;
      if (acc) { for (int i = 0; i < 512; ++i) buf[i] = yrot(acc[(size_t)tok * H + blk * 512 + i], usp[tok], tau[blk * 512 + i]);
        fwht512(buf);
        for (int i = 0; i < 512; ++i) hp[i] = rbf(hp[i] + rbf((buf[i] * K512) * so[blk * 512 + i])); }
      float a[128];
      for (int t = 0; t < 128; ++t) { float s = 0.f; for (int j = 0; j < 4; ++j) s = s + hp[t + 128 * j] * hp[t + 128 * j]; a[t] = s; }
      SS[tok * SST + blk] = tree128(a);
      for (int i = 0; i < 512; ++i) { const float u = hp[i] * (1.f + w[blk * 512 + i]); if (xu) xu[(size_t)tok * H + blk * 512 + i] = f2bf(u); buf[i] = u * sg_in[blk * 512 + i]; }
      a8_tail(buf, qq, al, MR, tok, blk, 10); });
  }
  // ---- Gated DeltaNet layer ----
  void gdn(Gdn& Y, int M, int n_commit) {
    const int MR = mr_of(M); const float e = eps;
    gemm_group({&Y.qkv, &Y.z}, M); mark("gdn: GEMM qkv+z");
    if (fast) {
      fx::k_gdn_post(q, M, fx::GdnPost{Y.qkv.part, Y.z.part, Y.qkv.tau, Y.z.tau, Y.qkv.sg_out, Y.z.sg_out, us, S, Y.wa, Y.wb, Y.A_log, Y.dt_bias, xn, Y.mixed, z, ab, Y.beta, Y.g, e}); mark("gdn: post (a/b, rotation)");
      fx::k_conv(q, M, Y.mixed, Y.conv, Y.hist, Y.q, Y.k, Y.v); mark("gdn: conv");
      state(Y, M, n_commit); mark("gdn: state recurrence");
      fx::k_gdn_out(q, M, MR, core, z, Y.gnorm, Y.out.sg_in, e, alpha, qx, us, Aw); aw_ready = true; mark("gdn: out norm");
      gemm_group({&Y.out}, M); mark("gdn: GEMM out"); return; }
    { const float *wa = Y.wa, *wb = Y.wb, *SS = S; const uint16_t* x = xn; float* abp = ab;                       // a / b projections
      q.parallel_for(sycl::range<1>(M * 96), [=](sycl::id<1> it) { const int tok = it[0] / 96, r = it[0] % 96; const float* W = r < 48 ? wb + (size_t)r * H : wa + (size_t)(r - 48) * H;
        float a[128];
        for (int t = 0; t < 128; ++t) { float s = 0.f; for (int j = 0; j < 5; ++j) for (int k = 0; k < 8; ++k) { const int i = 8 * (t + 128 * j) + k; s = s + W[i] * bf2f(x[(size_t)tok * H + i]); } a[t] = s; }
        abp[tok * 96 + r] = rbf(tree128(a) * rfac(SS + tok * SST, e)); }); }
    { const int64_t *aq = Y.qkv.part, *az = Y.z.part; const float *tq = Y.qkv.tau, *tz = Y.z.tau, *sq = Y.qkv.sg_out, *sz = Y.z.sg_out, *usp = us, *SS = S; uint16_t* mx = Y.mixed; float* zz = z;
      q.parallel_for(sycl::range<1>(M * 32), [=](sycl::id<1> it) { const int tok = it[0] / 32, b = it[0] % 32; float buf[512]; const float r = rfac(SS + tok * SST, e);
        if (b < 20) { for (int i = 0; i < 512; ++i) buf[i] = yrot_n(aq[(size_t)tok * CV + b * 512 + i], usp[tok], r, tq[b * 512 + i]);
          fwht512(buf); for (int i = 0; i < 512; ++i) mx[(size_t)tok * CV + b * 512 + i] = f2bf((buf[i] * K512) * sq[b * 512 + i]); }
        else { const int bb = b - 20; for (int i = 0; i < 512; ++i) buf[i] = yrot_n(az[(size_t)tok * ZN + bb * 512 + i], usp[tok], r, tz[bb * 512 + i]);
          fwht512(buf); for (int i = 0; i < 512; ++i) zz[(size_t)tok * ZN + bb * 512 + i] = rbf((buf[i] * K512) * sz[bb * 512 + i]); } }); }
    { const uint16_t* mx = Y.mixed; const float *cw = Y.conv, *hist = Y.hist, *abp = ab, *Al = Y.A_log, *dt = Y.dt_bias; float *Q = Y.q, *K = Y.k, *V = Y.v, *B = Y.beta, *Gg = Y.g;
      q.parallel_for(sycl::range<1>(M * 80), [=](sycl::id<1> it) { const int t = it[0] / 80, slot = it[0] % 80; float act[128];
        for (int d = 0; d < 128; ++d) { const int c = slot * 128 + d; float x[4];
          for (int k = 0; k < 4; ++k) { const int r = t - 3 + k; x[k] = r >= 0 ? bf2f(mx[(size_t)r * CV + c]) : hist[(size_t)(r + 3) * CV + c]; }
          const float acc = ((cw[c * 4] * x[0] + cw[c * 4 + 1] * x[1]) + cw[c * 4 + 2] * x[2]) + cw[c * 4 + 3] * x[3];
          act[d] = rbf(pm_silu(rbf(acc))); }
        if (slot < 32) { float sq[128]; for (int d = 0; d < 128; ++d) sq[d] = act[d] * act[d];
          const float rr = pm_rsqrt(tree128(sq) + 1e-6f);
          for (int d = 0; d < 128; ++d) { const float y = act[d] * rr; if (slot < 16) Q[(size_t)t * 2048 + slot * 128 + d] = y * 0.08838834764831845f; else K[(size_t)t * 2048 + (slot - 16) * 128 + d] = y; } }
        else { const int hd = slot - 32; for (int d = 0; d < 128; ++d) V[(size_t)t * ZN + hd * 128 + d] = act[d];
          B[t * 48 + hd] = rbf(pm_sigmoid(abp[t * 96 + hd]));
          Gg[t * 48 + hd] = (-pm_expf(Al[hd])) * pm_softplus(abp[t * 96 + 48 + hd] + dt[hd]); } }); }
    state(Y, M, n_commit);
    { const float *cr = core, *zz = z, *gw = Y.gnorm, *si = Y.out.sg_in; float* al = alpha; int8_t* qq = qx;
      q.parallel_for(sycl::range<1>(M * 12), [=](sycl::id<1> it) { const int tok = it[0] / 12, blk = it[0] % 12; float buf[512];
        for (int hd = 0; hd < 4; ++hd) { const float* c = cr + (size_t)tok * ZN + blk * 512 + hd * 128; float sq[128]; for (int j = 0; j < 128; ++j) sq[j] = c[j] * c[j];
          const float rh = pm_rsqrt(fdiv(tree128(sq), 128.f) + e);
          for (int j = 0; j < 128; ++j) { const int el = blk * 512 + hd * 128 + j; const float n2 = rbf(gw[j] * rbf(c[j] * rh));
            buf[hd * 128 + j] = rbf(n2 * pm_silu(zz[(size_t)tok * ZN + el])) * si[el]; } }
        a8_tail(buf, qq, al, MR, tok, blk, 12); }); }
    gemm_group({&Y.out}, M);
  }
  // state recurrence over the M rows (and the conv history when committing)
  void state(Gdn& Y, int M, int n_commit) {
    const float *Q = Y.q, *K = Y.k, *V = Y.v, *B = Y.beta, *Gg = Y.g; float *Sp = Y.S, *cr = core;
    if (fast) { fx::k_state(q, M, n_commit, Q, K, V, B, Gg, Sp, Sscr, cr, Y.mixed, Y.hist); return; }
    q.parallel_for(sycl::range<1>(48 * 128), [=](sycl::id<1> it) { const int hd = it[0] / 128, j = it[0] % 128, kh = hd / 3; float s[128];
      for (int i = 0; i < 128; ++i) s[i] = Sp[(size_t)hd * 16384 + i * 128 + j];
      for (int t = 0; t < M; ++t) { const float* kt = K + (size_t)t * 2048 + kh * 128; const float* qt = Q + (size_t)t * 2048 + kh * 128;
        const float eg = pm_expf(Gg[t * 48 + hd]); float kv[4], o[4];
        for (int w = 0; w < 4; ++w) { float a = 0.f; for (int r = 0; r < 32; ++r) { const int i = 32 * w + r; s[i] = s[i] * eg; a = a + s[i] * kt[i]; } kv[w] = a; }
        const float d = (V[(size_t)t * ZN + hd * 128 + j] - (((kv[0] + kv[1]) + kv[2]) + kv[3])) * B[t * 48 + hd];
        for (int w = 0; w < 4; ++w) { float a = 0.f; for (int r = 0; r < 32; ++r) { const int i = 32 * w + r; s[i] = s[i] + kt[i] * d; a = a + s[i] * qt[i]; } o[w] = a; }
        cr[(size_t)t * ZN + hd * 128 + j] = rbf(((o[0] + o[1]) + o[2]) + o[3]);
        if (t + 1 == n_commit) for (int i = 0; i < 128; ++i) Sp[(size_t)hd * 16384 + i * 128 + j] = s[i]; } });
    if (n_commit > 0) { const uint16_t* mx = Y.mixed; float* hist = Y.hist; const int n = n_commit;
      q.parallel_for(sycl::range<1>(CV), [=](sycl::id<1> it) { const int c = it[0]; float nw[3];
        for (int k = 0; k < 3; ++k) { const int r = n - 3 + k; nw[k] = r >= 0 ? bf2f(mx[(size_t)r * CV + c]) : hist[(size_t)(3 + r) * CV + c]; }
        for (int k = 0; k < 3; ++k) hist[(size_t)k * CV + c] = nw[k]; }); }
  }
  // ---- MLP: gate/up GEMMs -> silu(gate) * up -> quantised input of down ----
  void mlp(Mlp& Y, int M) {
    const int MR = mr_of(M); const float e = eps;
    gemm_group({&Y.gate, &Y.up}, M); mark("mlp: GEMM gate+up");
    if (fast) { fx::k_mlp_post(q, M, MR, fx::MlpPost{Y.gate.part, Y.up.part, Y.gate.tau, Y.up.tau, Y.gate.sg_out, Y.up.sg_out, Y.down.sg_in, S, us, alpha, qx, Aw, e}); aw_ready = true; }
    else { const int64_t *ag = Y.gate.part, *au = Y.up.part; const float *tg = Y.gate.tau, *tu = Y.up.tau, *sg = Y.gate.sg_out, *su = Y.up.sg_out, *si = Y.down.sg_in, *usp = us, *SS = S; float* al = alpha; int8_t* qq = qx;
      q.parallel_for(sycl::range<1>(M * 34), [=](sycl::id<1> it) { const int tok = it[0] / 34, blk = it[0] % 34; float bg[512], bu[512]; const float r = rfac(SS + tok * SST, e);
        for (int i = 0; i < 512; ++i) { const size_t n = (size_t)tok * F + blk * 512 + i; bg[i] = yrot_n(ag[n], usp[tok], r, tg[blk * 512 + i]); bu[i] = yrot_n(au[n], usp[tok], r, tu[blk * 512 + i]); }
        fwht512(bg); fwht512(bu);
        for (int i = 0; i < 512; ++i) { const float g = rbf((bg[i] * K512) * sg[blk * 512 + i]), u = rbf((bu[i] * K512) * su[blk * 512 + i]);
          bg[i] = rbf(rbf(pm_silu(g)) * u) * si[blk * 512 + i]; }
        a8_tail(bg, qq, al, MR, tok, blk, 34); }); }
    mark("mlp: post (silu, a8)"); gemm_group({&Y.down}, M); mark("mlp: GEMM down");
  }
  const float* first_sg(int) { return L[0].g.qkv.sg_in; }   // every 5120-column matrix has the same input signs (seed 7, offset 0)
};
}  // namespace yx
