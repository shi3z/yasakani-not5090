// SPDX-License-Identifier: Apache-2.0
// Drop-in engine library for the YASAKANI Python package (C ABI of include/yskn_abi.h), Intel Xe2 / SYCL.
// Stage (i)/(ii) of spec/D_spec_api.md section 5: no look-ahead yet (one committing row per pass), multi-row prefill.
#include "attn.hpp"
#include "engine.hpp"
#include "head.hpp"
#include "draft.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#define YX_API extern "C" __attribute__((visibility("default")))
namespace {
thread_local std::string g_err;
double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}
struct yskn_engine {
  yx::Engine E; yx::Head hd; std::vector<std::unique_ptr<yx::Attn>> A; int max_ctx; int pos = 0; std::vector<int> hist;   // hist = committed tokens (positions 0..pos-1)
  yx::SmpParams smp; std::atomic<int> cancel{0}; std::vector<float> lg; yx::Draft dr;     // dr: the draft model (off until yskn_load_bdraft)
  // device memory by purpose (bytes), for YX_MEM=1 and yskn_last_error-free diagnostics
  size_t mem_model = 0, mem_head = 0, mem_attn = 0, mem_draft = 0;
  static inline const int sysman_env = (setenv("ZES_ENABLE_SYSMAN", "1", 0), 0);      // before Level Zero starts: lets the driver report the free device memory
  void mem_report(const char* when) { if (!getenv("YX_MEM")) return; const double G = 1073741824.0;
    size_t tot = 0, fre = 0; try { auto d = E.q.get_device(); tot = d.get_info<sycl::info::device::global_mem_size>(); if (d.has(sycl::aspect::ext_intel_free_memory)) fre = d.get_info<sycl::ext::intel::info::device::free_memory>(); } catch (...) {}
    fprintf(stderr, "[xe mem] %s: batch %d, context %d | weights + engine scratch %.2f GB, head %.2f, attention (KV cache, mirrors, batch buffers) %.2f, draft model %.2f | allocated %.2f of %.2f GB, driver reports %.2f GB free | batch estimate %.2f GB against %.2f GB free at start\n",
            when, E.cap, max_ctx, mem_model / G, mem_head / G, mem_attn / G, mem_draft / G, yx::g_dev_bytes / G, tot / G, fre / G, E.mem_need / G, E.mem_avail / G); }
  size_t own0 = 0, own1 = 0;      // this engine's range of yx::g_dev_ptrs (one engine per process at a time)
  yskn_engine(const char* path, int mc, int cap) : E(path, yx::NL, cap), max_ctx((mc + 63) / 64 * 64), lg(yx::VOC) {
    const size_t b0 = yx::g_dev_bytes; (void)b0;
    mem_model = yx::g_dev_bytes; hd.init(E); mem_head = yx::g_dev_bytes - mem_model;
    for (int l = 3; l < yx::NL; l += 4) A.emplace_back(std::make_unique<yx::Attn>(E, l, max_ctx));
    E.q.wait(); mem_attn = yx::g_dev_bytes - mem_model - mem_head;
  }
  ~yskn_engine() { try { E.q.wait(); } catch (...) {} yx::dev_release(own0); yx::g_dev_bytes = 0; }
  void reset() { E.q.wait(); pos = 0; hist.clear(); dr.reset(); E.pend_rows = 0; E.gset = 0;
    for (auto& Y : E.L) if (!Y.full && Y.g.S) { E.q.memset(Y.g.S, 0, 48 * 128 * 128 * 4); E.q.memset(Y.g.hist, 0, 3 * yx::CV * 4); }
    E.q.wait(); }
  // one pass over M rows at positions pos..pos+M-1 (ids already in E.ids); commit advances the DeltaNet states
  void pass(int M, bool commit) {
    using namespace yx; const float* sg = E.first_sg(0);
    E.embed(M); E.res_norm(nullptr, M, E.L[0].ln1, sg, !E.L[0].full);
    for (int l = 0; l < NL; ++l) { Layer& Y = E.L[l];
      if (Y.full) { Attn& a = *A[l / 4]; attn_forward(E, a, M, pos); E.res_norm(&a.o, M, Y.ln2, Y.m.gate.sg_in, false); }
      else { E.gdn(Y.g, M, commit ? M : 0); E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); }
      E.mlp(Y.m, M);
      if (l + 1 < NL) E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, sg, !E.L[l + 1].full);
      else E.res_norm(&Y.m.down, M, hd.fnorm, hd.T.sg_in, false);
      if (dr.on && l % 14 == 5) { if (M <= MRMAX) E.q.memcpy(dr.cap + (size_t)(l / 14) * MRMAX * H, E.h, (size_t)M * H * 4);
        else { if (!pfcap) pfcap = E.dev<float>((size_t)5 * E.cap * H); E.q.memcpy(pfcap + (size_t)(l / 14) * E.cap * H, E.h, (size_t)M * H * 4); } } }      // drafter features: h after layers 5, 19, 33, 47, 61
  }
  // token predicted by row `row` of the last pass, which sits at absolute position p (the new token goes to p + 1)
  int pick(int M, int row, int p) { int t; hd.run(E, M, row, 1, &t); if (!smp.on) return t; hd.read_logits(E, row, lg.data()); return yx::Head::sample(lg.data(), smp, (int64_t)p + 1); }
  // tokens chosen by the main model after each of the M rows of the last pass (row r sits at position p0 + r)
  void picks(int M, int p0, int* am) { hd.run(E, M, 0, M, am);
    if (const char* mf = getenv("YX_MARGIN_LOG")) {      // debug: position, chosen id, its logit, runner-up id and logit, for every row
      FILE* f = fopen(mf, "a");
      for (int r = 0; r < M; ++r) { hd.read_logits(E, r, lg.data()); int i1 = -1, i2 = -1; float v1 = -3.4e38f, v2 = -3.4e38f;
        for (int v = 0; v < yx::VOC; ++v) { const float x = lg[v]; if (x > v1) { v2 = v1; i2 = i1; v1 = x; i1 = v; } else if (x > v2) { v2 = x; i2 = v; } }
        fprintf(f, "%d %d %.9g %d %.9g\n", p0 + r + 1, i1, v1, i2, v2); }
      fclose(f); }
    if (smp.on) for (int r = 0; r < M; ++r) { hd.read_logits(E, r, lg.data()); am[r] = yx::Head::sample(lg.data(), smp, (int64_t)p0 + r + 1); } }
  // big batch: move the head inputs (qx, Aw, us, S) of row `row` of an M-row pass to row 0 of a 1-row layout, so that the head runs on one row
  float* pfcap = nullptr; int8_t* rq = nullptr; float* rf = nullptr; int32_t* ra = nullptr;
  void row_to_front(int M, int row) {
    using namespace yx; if (!rq) { rq = E.dev<int8_t>(H); rf = E.dev<float>(1 + SST); ra = E.dev<int32_t>(10); }
    const int MR = Engine::mr_of(M); int8_t *qx = E.qx, *q1 = rq; float *us = E.us, *S = E.S, *f1 = rf; int32_t *Aw = E.Aw, *a1 = ra;
    E.q.parallel_for(sycl::range<1>(160), [=](sycl::id<1> it) { const int c = it[0]; for (int b = 0; b < 32; ++b) q1[c * 32 + b] = qx[((size_t)c * MR + row) * 32 + b];
      if (c == 0) { f1[0] = us[row]; for (int k = 0; k < SST; ++k) f1[1 + k] = S[row * SST + k]; for (int g = 0; g < 10; ++g) a1[g] = Aw[row * 10 + g]; } });
    E.q.parallel_for(sycl::range<1>(160), [=](sycl::id<1> it) { const int c = it[0]; for (int b = 0; b < 32; ++b) qx[c * 32 + b] = q1[c * 32 + b];
      if (c == 0) { us[0] = f1[0]; for (int k = 0; k < SST; ++k) S[k] = f1[1 + k]; for (int g = 0; g < 10; ++g) Aw[g] = a1[g]; } });
    E.aw_ready = true;
  }
  // append tokens (committing); returns the token predicted after the last one.  am (optional): the choice after every row.
  int feed(const int* ids, int n, int* am = nullptr) {
    if (pos + n > max_ctx) throw std::runtime_error("prefill: context overflow"); int t = -1; int tmp[yx::MRMAX];
    E.flush_states(); E.gset = 0;
    const int B = E.cap;                                                 // rows per committing pass (YX_PF_BATCH)
    for (int i = 0; i < n; i += B) { const int M = std::min(B, n - i);
      E.q.memcpy(E.ids, ids + i, M * 4).wait(); pass(M, true);
      if (M <= yx::MRMAX) {
        if (am) { picks(M, pos, am + i); t = am[i + M - 1]; }
        else if (i + M == n) { picks(M, pos, tmp); t = tmp[M - 1]; }     // head on all rows of the last chunk only
        dr.ctx(M, pos); }                                                 // drafter context rows of the chunk (no-op without drafter)
      else {
        if (dr.on) for (int j = 0; j < M; j += yx::MRMAX) { const int mj = std::min(yx::MRMAX, M - j);      // drafter context in its own 16-row chunks
          for (int c = 0; c < 5; ++c) E.q.memcpy(dr.cap + (size_t)c * yx::MRMAX * yx::H, pfcap + ((size_t)c * E.cap + j) * yx::H, (size_t)mj * yx::H * 4);
          dr.ctx(mj, pos + j); }
        if (am) { picks(M, pos, am + i); t = am[i + M - 1]; }                           // look-ahead tables need the model's choice after every prompt row
        else if (i + M == n) { row_to_front(M, M - 1); t = pick(1, 0, pos + M - 1); } }  // otherwise the head runs on the last row only
      pos += M; hist.insert(hist.end(), ids + i, ids + i + M); }
    return t;
  }
  // look-ahead: M rows (frontier + drafts) at pos..pos+M-1 without committing; am[r] = the model's choice after row r
  int last_M = 0; bool pre_committed = false; int vids[yx::MRMAX];
  void verify(const int* ids, int M, int* am) {
    if (M < 1 || M > yx::MRMAX || pos + M > max_ctx) throw std::runtime_error("verify: bad row count");
    const bool c1 = M == 1; E.q.memcpy(E.ids, ids, M * 4).wait(); std::copy(ids, ids + M, vids);
    E.gset = (E.pend_rows && E.pend_set == 0) ? 1 : 0;      // rows waiting for their replay stay where they are
    pass(M, c1); E.pend_rows = 0; picks(M, pos, am); pre_committed = c1; last_M = M;
  }
  void commit(int n_accept) {
    if (n_accept < 0 || n_accept >= last_M) throw std::runtime_error("commit: bad row count");
    const int rows = n_accept + 1;
    if (!pre_committed) {
      static const bool defer = E.fast && !getenv("YX_NO_DEFER");
      if (defer) { for (auto& Y : E.L) if (!Y.full && Y.g.S) yx::fx::k_hist(E.q, rows, Y.g.mixed, Y.g.hist);      // the conv history now; the states with the next pass
        E.pend_rows = rows; E.pend_set = E.gset; }
      else { const yx::fx::StateOld none; for (auto& Y : E.L) if (!Y.full && Y.g.S) { const bool s2 = E.gset != 0; yx::Gdn& g = Y.g;
          yx::fx::k_state(E.q, rows, rows, s2 ? g.q2 : g.q, s2 ? g.k2 : g.k, s2 ? g.v2 : g.v, s2 ? g.beta2 : g.beta, s2 ? g.g2 : g.g, g.S, E.Sscr, E.core, g.mixed, g.hist, none); } } }
    hist.insert(hist.end(), vids, vids + rows); pos += rows; last_M = 0;
  }
};
#define YX_TRY(body) try { body; return 0; } catch (const std::exception& ex) { g_err = ex.what(); return -1; }
// Bytes that engine + head + attention allocate for a prefill batch of `cap` rows: the same constructors in counting mode (exact).
static double dry_bytes(const char* path, int ctx, int cap) {
  const size_t keep = yx::g_dev_bytes; yx::g_dev_bytes = 0; yx::g_dry = true; double r = 0;
  try { yx::Engine e(path, yx::NL, cap); yx::Head h; h.init(e); std::vector<std::unique_ptr<yx::Attn>> at; for (int l = 3; l < yx::NL; l += 4) at.emplace_back(std::make_unique<yx::Attn>(e, l, ctx));      // kept alive together: the 16 layers share one set of buffers
    r = (double)yx::g_dev_bytes; } catch (...) { r = 0; }
  yx::g_dry = false; yx::g_dev_bytes = keep; return r;
}
// Prefill batch: YX_PF_BATCH if set; else the largest of 256 / 128 / 64 / 16 for which  engine(batch) + draft model + margin  fits into
// the device memory that is free now (other processes' use is already taken off by the driver).  engine(batch) is exact and linear in
// the batch (two counting runs); the draft model next to the model file takes 0.9 x its file sizes (measured 2.29 GB for 2.57 GB of
// files); margin: 0.15 GB (`YX_MEM_MARGIN_GB`) for kernels and runtime (measured about 0.1 GB).
struct BatchPlan { int cap = 16; double need = 0, avail = 0, base = 0, per_row = 0, draft = 0; };
static BatchPlan plan_batch(const char* path, int ctx) {
  BatchPlan P; const bool forced = getenv("YX_PF_BATCH") != nullptr;      // forced: the estimate is still made, for the report
  double total = 0, avail = 0;
  try { sycl::device d(sycl::gpu_selector_v); total = (double)d.get_info<sycl::info::device::global_mem_size>();
    if (d.has(sycl::aspect::ext_intel_free_memory)) avail = (double)d.get_info<sycl::ext::intel::info::device::free_memory>(); } catch (...) {}
  if (avail <= 0) avail = 0.93 * total;
  const double b16 = dry_bytes(path, ctx, 16), b256 = dry_bytes(path, ctx, 256);
  if (avail <= 0 || b16 <= 0 || b256 <= 0) { P.cap = forced ? yx::pf_batch() : 64; return P; }
  { std::error_code ec; for (auto& f : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path(), ec)) { const std::string n = f.path().filename().string();
      if (n.rfind("bdraft", 0) == 0 && (f.path().extension() == ".yskn" || f.path().extension() == ".shead")) P.draft += 0.9 * (double)f.file_size(ec); } }
  P.per_row = (b256 - b16) / 240.0; P.base = b16 - 16.0 * P.per_row + 0.2 * 1073741824.0; P.avail = avail;      // + 0.2 GB: the counting run comes out 0.13 GB below the real allocations (measured, all batches, both models)
  const double margin = (getenv("YX_MEM_MARGIN_GB") ? atof(getenv("YX_MEM_MARGIN_GB")) : 0.15) * 1073741824.0;
  for (int b : {256, 128, 64, 16}) { P.cap = b; P.need = P.base + P.per_row * b + P.draft; if (P.need + margin <= avail) break; }
  if (forced) { P.cap = yx::pf_batch(); P.need = P.base + P.per_row * P.cap + P.draft; }
  return P;
}
YX_API yskn_engine* yskn_open(const char* path, int max_ctx, int, int) {
  try { const int ctx = (max_ctx + 63) / 64 * 64; const BatchPlan P = plan_batch(path, ctx);
    for (int cap = P.cap;; cap = cap > 128 ? 128 : cap > 64 ? 64 : 16) {      // allocation failure: undo and retry with a smaller batch
      const size_t mark = yx::g_dev_ptrs.size(), bytes0 = yx::g_dev_bytes;
      try { auto* e = new yskn_engine(path, max_ctx, cap); e->own0 = mark; e->E.mem_need = P.need > 0 ? P.base + P.per_row * cap + P.draft : 0; e->E.mem_avail = P.avail; e->mem_report("open"); return e; }
      catch (const std::exception& ex) { yx::dev_release(mark); yx::g_dev_bytes = bytes0; if (cap <= 16 || std::string(ex.what()).find("out of device memory") == std::string::npos) throw;
        if (getenv("YX_MEM")) fprintf(stderr, "[xe mem] batch %d: %s; the partial allocations were freed, trying a smaller batch\n", cap, ex.what()); } } }
  catch (const std::exception& ex) { g_err = ex.what(); return nullptr; } }
YX_API void yskn_close(yskn_engine* e) { delete e; }
YX_API int yskn_reset(yskn_engine* e) { YX_TRY(e->reset()) }
YX_API int yskn_position(yskn_engine* e) { return e->pos; }
YX_API const char* yskn_last_error(void) { return g_err.c_str(); }
YX_API int yskn_cancel(yskn_engine* e) { e->cancel.store(1); return 0; }
YX_API int yskn_load_bdraft(yskn_engine* e, const char* path) { YX_TRY(const size_t b0 = yx::g_dev_bytes; e->dr.load(e->E, path, e->max_ctx); e->mem_draft = yx::g_dev_bytes - b0; e->mem_report("draft model loaded")) }
YX_API int yskn_set_opt(yskn_engine* e, int key, int value) { YX_TRY(
  switch (key) {
    case 10: e->smp.on = value > 0; if (value > 0) e->smp.temp = value / 1000.0; break;
    case 11: if (value < 1 || value > 1000) throw std::runtime_error("set_opt: top-p x 1000 must be 1..1000"); e->smp.top_p = value / 1000.0; break;
    case 12: if (value < 1 || value > 20) throw std::runtime_error("set_opt: top-k must be 1..20"); e->smp.top_k = value; break;
    case 13: e->smp.seed = (uint64_t)(uint32_t)value; break;
    case 16: if (value != 2 && value != 3) throw std::runtime_error("set_opt: sampler version must be 2 or 3"); e->smp.version = value; break;
    case 29: if (value < 0 || value > 1000) throw std::runtime_error("set_opt: min-p x 1000 must be 0..1000"); e->smp.min_p = value / 1000.0; break;
    default: throw std::runtime_error("set_opt: unknown key " + std::to_string(key)); }) }
YX_API int yskn_prefill(yskn_engine* e, const int32_t* ids, int n, int32_t* first) { YX_TRY(*first = e->feed(ids, n)) }
YX_API int yskn_decode(yskn_engine*, int32_t*, int) { g_err = "decode: not implemented in the Xe port (use spec_generate)"; return -1; }
YX_API int yskn_logits(yskn_engine* e, float* out) { YX_TRY(e->hd.read_logits(e->E, 0, out)) }
YX_API int yskn_nll(yskn_engine*, const int32_t*, int, float*) { g_err = "nll: not implemented in the Xe port"; return -1; }
// Hashes of the committed state (tests): [0] DeltaNet states, [1] convolution histories, [2] KV cache (codes and scales of the slots < position),
// [3] decoded K/V mirrors of the slots < position (0 without mirrors).  count (optional) = number of hashes written.
YX_API int yskn_state_hash(yskn_engine* e, uint64_t* out, int n, int* count) { try {
  using namespace yx; sycl::queue& q = e->E.q; e->E.flush_states(); q.wait(); uint64_t hsh[4] = {1469598103934665603ull, 1469598103934665603ull, 1469598103934665603ull, 1469598103934665603ull};
  std::vector<uint8_t> buf; auto add = [&](int k, const void* dev, size_t bytes) { if (!dev || !bytes) return; buf.resize(bytes); q.memcpy(buf.data(), dev, bytes).wait();
    uint64_t x = hsh[k]; const uint64_t* w = (const uint64_t*)buf.data(); for (size_t i = 0; i < bytes / 8; ++i) x = (x ^ w[i]) * 1099511628211ull; for (size_t i = bytes / 8 * 8; i < bytes; ++i) x = (x ^ buf[i]) * 1099511628211ull; hsh[k] = x; };
  for (auto& Y : e->E.L) if (!Y.full && Y.g.S) { add(0, Y.g.S, (size_t)48 * 128 * 128 * 4); add(1, Y.g.hist, (size_t)3 * CV * 4); }
  const int pos = e->pos, mc = e->max_ctx;
  for (auto& ap : e->A) { Attn& a = *ap; for (int h = 0; h < 4; ++h) { const size_t r0 = (size_t)h * mc;
      add(2, a.kp + r0 * 40, (size_t)pos * 160); add(2, a.kps + r0, (size_t)pos * 4);
      if (a.vpk) { add(2, a.vp + r0 * 40, (size_t)pos * 160); add(2, a.vps + r0, (size_t)pos * 4); } else { add(2, a.v8 + r0 * 256, (size_t)pos * 256); add(2, a.vs + r0, (size_t)pos * 4); }
      if (a.mk && pos >= 16) { const size_t g0 = (size_t)h * a.sh->G * 4096; add(3, a.mk + g0, (size_t)(pos / 16) * 8192); add(3, a.mv + g0, (size_t)(pos / 16) * 8192); } } }
  for (int i = 0; i < n && i < 4; ++i) out[i] = hsh[i]; if (count) *count = n < 4 ? n : 4; return 0; } catch (const std::exception& ex) { g_err = ex.what(); return -1; } }
namespace {
// Host draft sources of the reference engine (spec D 2.3): copy from the context (D0) and next-token tables (D1).
struct Drafts {
  std::vector<int> T; std::unordered_map<uint64_t, int> T2, T3; std::vector<std::unordered_map<uint64_t, int>> ng; int indexed = 0, nmin = 2, nmax = 8;
  bool bigram = true, trigram = true;
  Drafts() : T(yx::VOC, -1), ng(9) {}
  static uint64_t key2(int a, int b) { return (uint64_t)a * yx::VOC + b; }
  static uint64_t key3(int a, int b, int c) { return ((uint64_t)a * yx::VOC + b) * yx::VOC + c; }
  void record(int p2, int prev, int cur, int top) { if (cur < 0) return; T[cur] = top; if (bigram && prev >= 0) T2[key2(prev, cur)] = top; if (trigram && p2 >= 0 && prev >= 0) T3[key3(p2, prev, cur)] = top; }
  int next_tok(int p2, int prev, int cur) const {
    if (trigram && p2 >= 0 && prev >= 0) { auto it = T3.find(key3(p2, prev, cur)); if (it != T3.end()) return it->second; }
    if (bigram && prev >= 0) { auto it = T2.find(key2(prev, cur)); if (it != T2.end()) return it->second; }
    return cur >= 0 ? T[cur] : -1; }
  static uint64_t hash(const int* t, int n) { uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    for (int i = 0; i < n; ++i) { h ^= (uint64_t)t[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xBF58476D1CE4E5B9ull; } return h; }
  // longest n-gram ending at F that occurred before; chain = what followed it
  int d0(const std::vector<int>& S, int F, int gmax, std::vector<int>& chain) {
    for (; indexed < F; ++indexed) for (int n = nmin; n <= nmax; ++n) if (indexed - n + 1 >= 0) ng[n][hash(&S[indexed - n + 1], n)] = indexed;
    chain.clear();
    for (int n = nmax; n >= nmin; --n) { if (F - n + 1 < 0) continue; auto it = ng[n].find(hash(&S[F - n + 1], n)); if (it == ng[n].end()) continue; const int p = it->second;
      if (!std::equal(&S[p - n + 1], &S[p + 1], &S[F - n + 1])) continue;
      for (int i = p + 1; i <= std::min(F, p + gmax); ++i) chain.push_back(S[i]);
      return n; }
    return 0; }
};
}
YX_API int yskn_spec_generate(yskn_engine* e, const int32_t* prompt, int n_prompt, int n_gen, const int32_t* cfg, int n_cfg, int32_t* out_ids, double* stats, double* times_ms) {
  try {
    if (n_cfg < 12) throw std::runtime_error("spec_generate: need at least 12 config values");
    if (n_prompt < 1) throw std::runtime_error("spec_generate: empty prompt");
    auto cf = [&](int i, int d) { return i < n_cfg ? cfg[i] : d; };
    const int gamma = cf(0, 7), use_d0 = cf(1, 1), use_d1 = cf(2, 1), d0_prio = cf(5, 3), extend = cf(6, 1), d1_rej = cf(7, 1), gamma_long = cf(17, 15), long_n = cf(18, 6), long_d0 = cf(22, 8);
    if (gamma < 0 || gamma > 15) throw std::runtime_error("spec_generate: gamma must be 0..15");
    e->cancel.store(0);
    const int eos1 = cf(20, -1), eos2 = cf(21, -1); auto is_eos = [&](int t) { return t >= 0 && (t == eos1 || t == eos2); };
    const bool nola = getenv("YX_NO_LOOKAHEAD") != nullptr, look = (use_d0 || use_d1) && !nola;       // look: host draft sources (D0 copy, D1 tables)
    const bool bdr = cf(36, 1) && e->dr.on && !nola;                                                  // the draft model (spec D 2.4)
    const int bdr_long_n = cf(37, 4), bdr_adapt = cf(38, 1), bdr_probe = cf(39, 4), bdr_d0_len = cf(40, 15);
    Drafts D; D.nmin = cf(3, 2); D.nmax = std::min(8, cf(4, 8)); D.bigram = cf(12, 1); D.trigram = cf(19, 1);
    const double t0 = now_ms();
    // prefix reuse, mode 1 only: continue when the prompt strictly extends what the state holds
    int start = 0, mode = 0;
    if ((int)e->hist.size() < n_prompt && !e->hist.empty() && std::equal(e->hist.begin(), e->hist.end(), prompt)) { start = (int)e->hist.size(); mode = 1; }
    else e->reset();
    std::vector<int> S(prompt, prompt + n_prompt), am(n_prompt + 16);
    int tok = e->feed(prompt + start, n_prompt - start, look ? am.data() + start : nullptr); out_ids[0] = tok;
    if (look) for (int p = start; p < n_prompt; ++p) D.record(p >= 2 ? S[p - 2] : -1, p >= 1 ? S[p - 1] : -1, S[p], am[p]);
    S.push_back(tok);
    const double t1 = now_ms(); int produced = 0, passes = 0; bool stop = is_eos(tok), full_prev = false; std::vector<int> chain, src; int rows_hist[17] = {};
    long drafted[3] = {0, 0, 0}, accepted[3] = {0, 0, 0}; int chain_none = 0, bdr_rounds = 0, n_bdr_d0 = 0, n_probes = 0, n_longm = 0;
    double ms_verify = 0, ms_draft = 0, ms_commit = 0, ms_ctx = 0;
    // drafter-vs-copy running averages, pending comparisons, context rows owed to the drafter, 8/16-row controller
    struct Pend { int F, which; std::vector<int> ch; }; std::vector<Pend> dpend; double dema[2] = {3.0, 3.0}; int dprobe = 0, dctx_n = 0, dctx_pos = 0;
    const double b16_ratio = 0.22, b16_decay = 0.9, b16_full = 0.45, b16_floor = 0.6, b16_decay8 = 0.97; const int b16_probe = 256, b16_max = 256, b16_full_since = 4;
    double b16_ema = 0.0, b8_ema = 4.0, full8_ema = 0.0; int b16_since = 0, b16_n = 0, b16_probe_cur = b16_probe;
    const int blk_mode = getenv("YX_BLK16") ? atoi(getenv("YX_BLK16")) : 3;       // 0: 8-row blocks only, 1: always 16, 2: the reference controller, 3: cost-based (default)
    // Mode 3 (a speed policy only: the tokens are the main model's in every mode).  On this GPU a 16-row pass costs c16 x an 8-row pass
    // (measured 1.12), so 16 rows pay when the tokens they add per pass, g16, exceed (c16 - 1) x the tokens of an 8-row pass.
    // g16 is observed directly in 16-row passes (accepted beyond 7) and estimated in 8-row passes: a fully accepted block would
    // have gone on for `extra` more tokens (the mean seen in 16-row passes), any other block for none.
    const double c16 = getenv("YX_B16_COST") ? atof(getenv("YX_B16_COST")) : 1.12, g_decay = getenv("YX_B16_DECAY") ? atof(getenv("YX_B16_DECAY")) : 0.85;
    double g16_ema = 0.0, extra_ema = getenv("YX_B16_EXTRA") ? atof(getenv("YX_B16_EXTRA")) : 3.0, t8_ema = 4.0;
    auto b16_observe = [&](int a_obs) { const double v = std::max(0, a_obs - 7);
      b16_ema = (b16_n++ == 0) ? v : b16_decay * b16_ema + (1.0 - b16_decay) * v; b16_since = 0;
      b16_probe_cur = b16_ema >= std::max(b16_floor, b16_ratio * b8_ema) ? b16_probe : std::min(b16_max, b16_probe_cur * 2); };
    int acc_hist[17] = {};
    while (produced < n_gen && !stop && !e->cancel.load()) {
      const int F = (int)S.size() - 1, xF = S[F], xP = F >= 1 ? S[F - 1] : -1, xQ = F >= 2 ? S[F - 2] : -1;
      const int room = std::min(n_gen - produced - 1, e->max_ctx - e->pos - 1);
      chain.clear(); src.clear(); bool bdr_dev = false, bdr_longm = false, bdr_d0 = false, b16_pass = false;
      const int g_short = std::max(0, std::min(gamma, room)), g_all = std::max(0, std::min(std::max(gamma, gamma_long), room));
      std::vector<int> c0; const int n0 = (look && use_d0 && g_all > 0) ? D.d0(S, F, g_all, c0) : 0;
      const bool lng = gamma_long > gamma && ((full_prev && n0 >= long_n) || (long_d0 > 0 && n0 >= long_d0));
      const int g_eff = std::min(lng ? g_all : g_short, bdr ? std::max(15, bdr_d0_len) : 15); if ((int)c0.size() > g_eff) c0.resize(g_eff);
      auto ext = [&] { while ((int)chain.size() < g_eff) { const int L = (int)chain.size(); auto at = [&](int k) { const int i = L - 1 - k; return i >= 0 ? chain[i] : (i == -1 ? xF : (i == -2 ? xP : xQ)); };
          const int t = D.next_tok(at(2), at(1), at(0)); if (t < 0) break; chain.push_back(t); src.push_back(1); } };
      auto take_d0 = [&] { chain = c0; src.assign(chain.size(), 0); if (extend && use_d1 && !chain.empty()) ext(); };
      if (bdr) {
        int kdfl = std::min(7, g_eff); const int room16 = std::min(15, room);
        if (blk_mode == 1 && room16 > 7) { kdfl = room16; b16_pass = true; }
        else if (blk_mode == 3 && room16 > 7 && g16_ema > (c16 - 1.0) * t8_ema) { kdfl = room16; b16_pass = true; }
        else if (blk_mode == 2 && room16 > 7 && (b16_ema >= std::max(b16_floor, b16_ratio * b8_ema) || b16_since >= b16_probe_cur || (full8_ema >= b16_full && b16_since >= b16_full_since))) { kdfl = room16; b16_pass = true; }
        if (g_eff > 0) {
          bdr_longm = n0 >= bdr_long_n && !c0.empty(); n_longm += bdr_longm;
          bdr_d0 = bdr_longm && (bdr_adapt == 2 || (bdr_adapt == 1 && dema[1] > dema[0]));
          const bool probe = bdr_d0 && bdr_adapt == 1 && bdr_probe > 0 && ++dprobe % bdr_probe == 0, run = !bdr_d0 || probe;
          if (probe && blk_mode && room16 > 7) { kdfl = room16; b16_pass = true; }
          const double td = now_ms(); int dr_out[16];
          e->dr.step(dctx_n, dctx_pos, run ? (b16_pass ? 16 : 8) : 0, xF, e->pos, e->smp, dr_out); dctx_n = 0;       // catch-up of the context, then the draft block
          ms_draft += now_ms() - td;
          if (bdr_d0) { if ((int)c0.size() > bdr_d0_len) c0.resize(bdr_d0_len);
            take_d0(); ++n_bdr_d0;
            if (probe) { dpend.push_back(Pend{F, 0, std::vector<int>(dr_out, dr_out + kdfl)}); ++n_probes; } }
          else { chain.assign(dr_out, dr_out + kdfl); src.assign(kdfl, 2); bdr_dev = true; ++bdr_rounds;
            if (bdr_longm) dpend.push_back(Pend{F, 1, std::vector<int>(c0.begin(), c0.begin() + std::min<int>((int)c0.size(), bdr_d0_len))}); }
        }
        if (dctx_n) { const double tx = now_ms(); e->dr.ctx(dctx_n, dctx_pos); dctx_n = 0; ms_ctx += now_ms() - tx; }      // no draft this pass: still feed the context
      } else if (look && g_eff > 0) { if (n0 >= d0_prio) take_d0(); else if (use_d1 && D.next_tok(xQ, xP, xF) >= 0) ext(); else if (n0 > 0) take_d0(); }
      const int kch = (int)chain.size(), M = kch + 1; int ids[yx::MRMAX], amv[yx::MRMAX]; ids[0] = xF; for (int j = 0; j < kch; ++j) ids[1 + j] = chain[j];
      const double tv = now_ms(); e->verify(ids, M, amv); ms_verify += now_ms() - tv;
      int a = 0; while (a < kch && chain[a] == amv[a]) ++a;
      if (blk_mode == 2 && bdr_dev) { b8_ema = b16_decay * b8_ema + (1.0 - b16_decay) * (std::min(a, 7) + 1);
        if (b16_pass) b16_observe(a); else { ++b16_since; full8_ema = 0.75 * full8_ema + 0.25 * (a >= 7 ? 1.0 : 0.0); b16_ema *= b16_decay8; } }
      if (blk_mode == 3 && bdr_dev) { t8_ema = g_decay * t8_ema + (1.0 - g_decay) * (std::min(a, 7) + 1); double gobs = 0.0;
        if (b16_pass) { gobs = std::max(0, a - 7); if (a >= 7) extra_ema = 0.8 * extra_ema + 0.2 * (a - 7); } else if (a >= 7) gobs = extra_ema;
        g16_ema = g_decay * g16_ema + (1.0 - g_decay) * gobs; }
      if (bdr_dev) ++acc_hist[a];
      const int a_all = a; if (bdr_longm) { const int w = bdr_d0 ? 1 : 0; dema[w] += (a_all - dema[w]) / 16.0; }
      for (int j = 0; j < a_all; ++j) if (is_eos(chain[j])) { a = j; stop = true; break; }
      if (!stop && is_eos(amv[a])) stop = true;
      auto in_tok = [&](int j) { return j >= 0 ? ids[j] : (j == -1 ? xP : xQ); };
      if (look) { if (d1_rej) for (int j = a + 1; j <= kch; ++j) D.record(in_tok(j - 2), in_tok(j - 1), ids[j], amv[j]);
                  for (int j = 0; j <= a; ++j) D.record(in_tok(j - 2), in_tok(j - 1), ids[j], amv[j]); }
      full_prev = kch > 0 && a == kch;
      const double tc = now_ms(); e->commit(a); if (e->dr.prof) e->E.q.wait(); ms_commit += now_ms() - tc;      // YX_DPROF: charge the state replay to the commit, not to the next drafter step
      if (bdr) { dctx_n = a + 1; dctx_pos = F; }                                    // the captured rows 0..a go to the drafter at its next step
      for (int j = 0; j < a; ++j) { S.push_back(chain[j]); if (produced < n_gen) out_ids[1 + produced++] = chain[j]; }
      S.push_back(amv[a]); if (produced < n_gen) out_ids[1 + produced++] = amv[a];
      for (size_t i = 0; i < dpend.size();) {                                       // what would the other source have scored
        const Pend& p = dpend[i]; int k = 0; bool done = false;
        for (; k < (int)p.ch.size(); ++k) { const int qq = p.F + 1 + k; if (qq >= (int)S.size()) break; if (S[qq] != p.ch[k]) { done = true; break; } }
        if (k == (int)p.ch.size()) done = true;
        if (done) { dema[p.which] += (k - dema[p.which]) / 16.0;
          if (blk_mode == 2 && p.which == 0 && (int)p.ch.size() > 7) { b8_ema = b16_decay * b8_ema + (1.0 - b16_decay) * (std::min(k, 7) + 1); b16_observe(k); }
          dpend.erase(dpend.begin() + (std::ptrdiff_t)i); } else ++i; }
      ++passes; ++rows_hist[M]; if (!kch) ++chain_none; for (int k = 0; k < kch; ++k) { ++drafted[src[k]]; if (k < a) ++accepted[src[k]]; }
    }
    if (bdr && dctx_n) { const double tx = now_ms(); e->dr.ctx(dctx_n, dctx_pos); e->E.q.wait(); ms_ctx += now_ms() - tx; }      // flush: the rings are complete for the next call
    const double t2 = now_ms(); times_ms[0] = t1 - t0; times_ms[1] = t2 - t1;
    stats[0] = passes; stats[1] = produced; stats[2] = chain_none; stats[3] = drafted[0]; stats[4] = accepted[0]; stats[5] = drafted[1]; stats[6] = accepted[1];
    for (int m = 1; m <= 16; ++m) stats[6 + m] = rows_hist[m];
    stats[23] = drafted[2]; stats[24] = accepted[2]; stats[25] = ms_verify; stats[26] = ms_draft; stats[28] = ms_commit;
    stats[27] = t2 - t1; stats[30] = std::min(produced, n_gen); stats[31] = stop ? 1 : 0; stats[35] = 0; stats[36] = -1; stats[37] = bdr_rounds; stats[38] = n_bdr_d0; stats[39] = n_probes; stats[40] = ms_ctx;
    stats[41] = mode; stats[42] = start;
    if (getenv("YX_DSTAT")) { yx::Draft& d = e->dr;
      fprintf(stderr, "[xe] prompt %d (reused %d) gen %d passes %d (%.2f tok/pass) | drafter %d rounds, drafted %ld acc %ld | d0 %ld/%ld d1 %ld/%ld bdr_d0 %d probes %d longm %d dema %.2f %.2f | ms: prefill %.0f verify %.1f draft %.1f ctx %.1f commit %.1f total %.1f\n",
              n_prompt, start, produced, passes, passes ? (produced + 1.0) / passes : 0.0, bdr_rounds, drafted[2], accepted[2], accepted[0], drafted[0], accepted[1], drafted[1], n_bdr_d0, n_probes, n_longm, dema[0], dema[1],
              t1 - t0, ms_verify, ms_draft, ms_ctx, ms_commit, t2 - t1);
      fprintf(stderr, "[xe] accepted-length histogram of drafter passes:"); for (int i = 0; i <= 15; ++i) fprintf(stderr, " %d", acc_hist[i]);
      fprintf(stderr, " | rows:"); for (int m = 1; m <= 16; ++m) fprintf(stderr, " %d", rows_hist[m]); fprintf(stderr, "\n");
      if (d.prof) d.report(stderr); }
    return 0;
  } catch (const std::exception& ex) { g_err = ex.what(); return -1; }
}
