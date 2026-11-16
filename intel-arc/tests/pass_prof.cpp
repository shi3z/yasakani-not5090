// Whole-pass timing (64 layers + head) at a given context length, with a per-component breakdown.
#include "../src/attn.hpp"
#include "../src/engine.hpp"
#include "../src/head.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
using namespace yx;
int main(int argc, char** argv) {
  const int ctx = argc > 1 ? atoi(argv[1]) : 256, max_ctx = argc > 2 ? atoi(argv[2]) : 8192;
  std::ifstream f("work/ids.bin", std::ios::binary); std::vector<int> base(12); f.read((char*)base.data(), 48);
  Engine E("models/speed/qwen38_27b_speed.yskn", NL); Head hd; hd.init(E);
  std::vector<std::unique_ptr<Attn>> A; for (int l = 3; l < NL; l += 4) A.emplace_back(std::make_unique<Attn>(E, l, max_ctx)); E.q.wait();
  std::map<std::string, double> T; bool prof = false; int pos = 0;
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  auto tm = [&](const char* n, auto&& fn) { if (!prof) { fn(); return; } const double t0 = now(); fn(); E.q.wait(); T[n] += now() - t0; };
  auto pass = [&](int M, bool commit) { const float* sg = E.first_sg(0);
    tm("embed+res_norm", [&] { E.embed(M); E.res_norm(nullptr, M, E.L[0].ln1, sg, !E.L[0].full); });
    for (int l = 0; l < NL; ++l) { Layer& Y = E.L[l];
      if (Y.full) { Attn& a = *A[l / 4]; tm("attention (16 layers)", [&] { attn_forward(E, a, M, pos); }); tm("res_norm x128", [&] { E.res_norm(&a.o, M, Y.ln2, Y.m.gate.sg_in, false); }); }
      else { tm("gdn (48 layers)", [&] { E.gdn(Y.g, M, commit ? M : 0); }); tm("res_norm x128", [&] { E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); }); }
      tm("mlp (64 layers)", [&] { E.mlp(Y.m, M); });
      tm("res_norm x128", [&] { if (l + 1 < NL) E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, sg, !E.L[l + 1].full); else E.res_norm(&Y.m.down, M, hd.fnorm, hd.T.sg_in, false); }); } };
  std::vector<int> ids(512); int am[16];
  { const int B = E.cap; for (; pos + B <= ctx; pos += B) { for (int i = 0; i < B; ++i) ids[i] = base[(pos + i) % 12]; E.q.memcpy(E.ids, ids.data(), B * 4).wait(); pass(B, true); } }     // context with the prefill batch
  E.q.wait(); std::printf("context %d\n", pos);
  const int only = getenv("YX_PP_M") ? atoi(getenv("YX_PP_M")) : 0;          // YX_PP_M=256: only this row count
  // YX_PP_ATTN="0,1,2,...": for the row count YX_PP_M, time the attention stage of a pass with each attention experiment (median of 5, with waits)
  if (getenv("YX_PP_ATTN") && only) { const int M = only; for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    std::string lst = getenv("YX_PP_ATTN"); size_t a0 = 0;
    while (a0 < lst.size()) { size_t a1 = lst.find(',', a0); if (a1 == std::string::npos) a1 = lst.size(); g_attn_mode = atoi(lst.substr(a0, a1 - a0).c_str()); a0 = a1 + 1;
      std::vector<double> ts, tk, tp; for (int r = 0; r < 6; ++r) { E.prof = true; E.T.clear(); E.mark(nullptr); const double f0 = now(); pass(M, false); E.q.wait(); const double f1 = now(); E.prof = false;
        double t = 0; for (auto& kv : E.T) if (kv.first.find("scores") != std::string::npos || kv.first.find("values") != std::string::npos || kv.first.find("fused") != std::string::npos) t += kv.second;
        if (r) { ts.push_back(t); tp.push_back(f1 - f0); } }
      std::sort(ts.begin(), ts.end()); std::sort(tp.begin(), tp.end());
      std::printf("attention experiment %2d  context %6d  rows %d:  scores + values of 16 layers %8.2f ms (median of 5; min %.2f, max %.2f)   whole pass with waits %.1f ms\n", g_attn_mode, pos, M, ts[2], ts[0], ts[4], tp[2]); std::fflush(stdout); }
    return 0; }
  for (int M : {1, 8, 16, 32, 64, 128, 256, 512}) { if (M > E.cap) break; if (only && M != only) continue; for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    double best = 1e9, bh = 1e9;
    for (int r = 0; r < 6; ++r) { double t0 = now(); pass(M, false); E.q.wait(); double t1 = now(); if (M <= 16) hd.run(E, M, 0, M, am); double t2 = now(); best = std::min(best, t1 - t0); bh = std::min(bh, t2 - t1); }
    prof = true; T.clear(); pass(M, false); prof = false;
    if (getenv("YX_FINE")) { E.prof = true; E.T.clear(); E.mark(nullptr); const double f0 = now(); pass(M, false); E.q.wait(); const double f1 = now(); E.prof = false;
      std::printf("  fine profile M=%d (pass with waits %.1f ms):\n", M, f1 - f0); for (auto& kv : E.T) std::printf("    %-28s %8.2f ms  %5.1f%%\n", kv.first.c_str(), kv.second, 100 * kv.second / (f1 - f0)); }
    std::printf("M=%2d: pass %.1f ms + head (all rows, arg-max) %.2f ms;  breakdown with waits:", M, best, bh);
    double s = 0; for (auto& kv : T) { std::printf("  %s %.1f", kv.first.c_str(), kv.second); s += kv.second; } std::printf("  (sum %.1f)\n", s); }
}
