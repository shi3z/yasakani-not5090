// Timing of the GEMM launch groups of one layer for a given number of rows:  gemm_big_test <rows> [reps]   (env YX_TR, YX_PF_BATCH)
#include "../src/engine.hpp"
#include <chrono>
#include <cstdio>
using namespace yx;
int main(int argc, char** argv) {
  const int M = argc > 1 ? atoi(argv[1]) : 16, reps = argc > 2 ? atoi(argv[2]) : 20;
  const char* md = getenv("YX_MODEL"); Engine E(md ? md : "models/speed/qwen38_27b_speed.yskn", 1);
  std::vector<int> ids(E.cap); for (int i = 0; i < E.cap; ++i) ids[i] = 1000 + 37 * i; E.q.memcpy(E.ids, ids.data(), E.cap * 4).wait();
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  Layer& Y = E.L[0]; E.embed(M); E.res_norm(nullptr, M, Y.ln1, E.first_sg(0), true); E.q.wait();
  auto tm = [&](const char* n, auto&& fn) { double best = 1e9; for (int r = 0; r < reps; ++r) { E.aw_ready = true; const double t0 = now(); fn(); E.q.wait(); best = std::min(best, now() - t0); }
    std::printf("  %s %.3f ms", n, best); };
  std::printf("rows %d:", M);
  tm("gate+up", [&] { E.gemm_group({&Y.m.gate, &Y.m.up}, M); });
  tm("qkv+z", [&] { E.gemm_group({&Y.g.qkv, &Y.g.z}, M); });
  // 17408-column input for down: run the MLP post once
  E.aw_ready = true; E.mlp(Y.m, M); E.q.wait();
  { const int MR = Engine::mr_of(M); (void)MR; }
  tm("down", [&] { E.gemm_group({&Y.m.down}, M); });
  int64_t cs = 0; { std::vector<int64_t> v((size_t)M * H); E.q.memcpy(v.data(), Y.m.down.part, v.size() * 8).wait(); for (auto x : v) cs = cs * 1000003 + x; }
  std::printf("  checksum(down) %lld\n", (long long)cs);
}
