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
  yskn_engine(const char* path, int mc) : E(path, yx::NL), max_ctx((mc + 63) / 64 * 64), lg(yx::VOC) {
    hd.init(E);
    for (int l = 3; l < yx::NL; l += 4) A.emplace_back(std::make_unique<yx::Attn>(E, l, max_ctx));
    E.q.wait();
  }
  void reset() { E.q.wait(); pos = 0; hist.clear(); dr.reset();
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
      if (dr.on && l % 14 == 5) E.q.memcpy(dr.cap + (size_t)(l / 14) * MRMAX * H, E.h, (size_t)M * H * 4); }      // drafter features: h after layers 5, 19, 33, 47, 61
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
  // append tokens (committing); returns the token predicted after the last one.  am (optional): the choice after every row.
  int feed(const int* ids, int n, int* am = nullptr) {
    if (pos + n > max_ctx) throw std::runtime_error("prefill: context overflow"); int t = -1; int tmp[yx::MRMAX];
    for (int i = 0; i < n; i += yx::MRMAX) { const int M = std::min(yx::MRMAX, n - i);
      E.q.memcpy(E.ids, ids + i, M * 4).wait(); pass(M, true);
      if (am) { picks(M, pos, am + i); t = am[i + M - 1]; }
      else if (i + M == n) { picks(M, pos, tmp); t = tmp[M - 1]; }       // head on all rows of the last chunk only
      dr.ctx(M, pos);                                                     // drafter context rows of the chunk (no-op without drafter)
      pos += M; hist.insert(hist.end(), ids + i, ids + i + M); }
    return t;
  }
  // look-ahead: M rows (frontier + drafts) at pos..pos+M-1 without committing; am[r] = the model's choice after row r
  int last_M = 0; bool pre_committed = false; int vids[yx::MRMAX];
  void verify(const int* ids, int M, int* am) {
    if (M < 1 || M > yx::MRMAX || pos + M > max_ctx) throw std::runtime_error("verify: bad row count");
    const bool c1 = M == 1; E.q.memcpy(E.ids, ids, M * 4).wait(); std::copy(ids, ids + M, vids);
    pass(M, c1); picks(M, pos, am); pre_committed = c1; last_M = M;
  }
  void commit(int n_accept) {
    if (n_accept < 0 || n_accept >= last_M) throw std::runtime_error("commit: bad row count");
    const int rows = n_accept + 1;
    if (!pre_committed) for (auto& Y : E.L) if (!Y.full && Y.g.S) E.state(Y.g, rows, rows);     // replay the DeltaNet states over the accepted rows
    hist.insert(hist.end(), vids, vids + rows); pos += rows; last_M = 0;
  }
};
#define YX_TRY(body) try { body; return 0; } catch (const std::exception& ex) { g_err = ex.what(); return -1; }
YX_API yskn_engine* yskn_open(const char* path, int max_ctx, int, int) { try { return new yskn_engine(path, max_ctx); } catch (const std::exception& ex) { g_err = ex.what(); return nullptr; } }
YX_API void yskn_close(yskn_engine* e) { delete e; }
YX_API int yskn_reset(yskn_engine* e) { YX_TRY(e->reset()) }
YX_API int yskn_position(yskn_engine* e) { return e->pos; }
YX_API const char* yskn_last_error(void) { return g_err.c_str(); }
YX_API int yskn_cancel(yskn_engine* e) { e->cancel.store(1); return 0; }
YX_API int yskn_load_bdraft(yskn_engine* e, const char* path) { YX_TRY(e->dr.load(e->E, path, e->max_ctx)) }
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
YX_API int yskn_state_hash(yskn_engine*, uint64_t*, int, int*) { g_err = "state_hash: not implemented in the Xe port"; return -1; }
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
    const int blk_mode = getenv("YX_BLK16") ? atoi(getenv("YX_BLK16")) : 2;       // 0: 8-row blocks only, 1: always 16, 2: adaptive (default)
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
