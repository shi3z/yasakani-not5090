// Whole-pass timing (64 layers + head) at a given context length, with a per-component breakdown.
#include "../src/attn.hpp"
#include "../src/engine.hpp"
#include "../src/head.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
using namespace yx;
int main(int argc, char** argv) {
  const int ctx = argc > 1 ? atoi(argv[1]) : 256, max_ctx = 8192;
  std::ifstream f("/data/yasakani/tmp/ids.bin", std::ios::binary); std::vector<int> base(12); f.read((char*)base.data(), 48);
  Engine E("/data/yasakani/model-quality/qwen38_27b_quality.yskn", NL); Head hd; hd.init(E);
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
  std::vector<int> ids(16); int am[16];
  for (; pos + 16 <= ctx; pos += 16) { for (int i = 0; i < 16; ++i) ids[i] = base[(pos + i) % 12]; E.q.memcpy(E.ids, ids.data(), 64).wait(); pass(16, true); }
  E.q.wait(); std::printf("context %d\n", pos);
  for (int M : {1, 8, 16}) { for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    double best = 1e9, bh = 1e9;
    for (int r = 0; r < 6; ++r) { double t0 = now(); pass(M, false); E.q.wait(); double t1 = now(); hd.run(E, M, 0, M, am); double t2 = now(); best = std::min(best, t1 - t0); bh = std::min(bh, t2 - t1); }
    prof = true; T.clear(); pass(M, false); prof = false;
    std::printf("M=%2d: pass %.1f ms + head (all rows, arg-max) %.2f ms;  breakdown with waits:", M, best, bh);
    double s = 0; for (auto& kv : T) { std::printf("  %s %.1f", kv.first.c_str(), kv.second); s += kv.second; } std::printf("  (sum %.1f)\n", s); }
}
