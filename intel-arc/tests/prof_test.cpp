// Per-kernel timing of one GDN layer + MLP (each step followed by q.wait()).
#include "../src/engine.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <map>
int main(int argc, char** argv) {
  const int M = atoi(argv[1]);
  std::ifstream f("/data/yasakani/tmp/ids.bin", std::ios::binary); std::vector<int> ids(16); f.read((char*)ids.data(), 48);
  yx::Engine E("/data/yasakani/model-speed/qwen38_27b_speed.yskn", 1); E.q.memcpy(E.ids, ids.data(), M * 4).wait();
  using namespace yx; std::map<std::string, double> T; std::vector<std::string> order;
  auto tm = [&](const char* n, auto&& fn) { auto t0 = std::chrono::steady_clock::now(); fn(); E.q.wait(); T[n] += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); if (T.size() > order.size()) order.push_back(n); };
  Layer& Y = E.L[0]; const int R = 20;
  for (int rep = 0; rep < R + 1; ++rep) { if (rep == 1) T.clear(), order.clear();
    tm("embed", [&] { E.embed(M); }); tm("res_norm(emb)", [&] { E.res_norm(nullptr, M, Y.ln1, E.first_sg(0), true); });
    tm("gemm qkv+z (+prep)", [&] { E.gemm_group({&Y.g.qkv, &Y.g.z}, M); });
    tm("gemm out (+prep)", [&] { E.gemm_group({&Y.g.out}, M); });
    tm("res_norm(out)", [&] { E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); });
    tm("gemm gate+up (+prep)", [&] { E.gemm_group({&Y.m.gate, &Y.m.up}, M); });
    tm("gemm down (+prep)", [&] { E.gemm_group({&Y.m.down}, M); });
    tm("state", [&] { E.state(Y.g, M, M); });
    tm("whole gdn()", [&] { E.gdn(Y.g, M, M); });
    tm("whole mlp()", [&] { E.mlp(Y.m, M); });
    tm("empty kernel", [&] { float* p = E.us; E.q.parallel_for(sycl::range<1>(1), [=](sycl::id<1>) { p[15] = 0.f; }); });
    if (E.fast) { Gdn& G = Y.g; const int MR = Engine::mr_of(M);      // the ESIMD kernels one by one (inputs: whatever the steps above left)
      tm("fx gdn_post (ab,qkv,z)", [&] { fx::k_gdn_post(E.q, M, fx::GdnPost{G.qkv.part, G.z.part, G.qkv.tau, G.z.tau, G.qkv.sg_out, G.z.sg_out, E.us, E.S, G.wa, G.wb, G.A_log, G.dt_bias, E.xn, G.mixed, E.z, E.ab, G.beta, G.g, E.eps}); });
      tm("fx conv", [&] { fx::k_conv(E.q, M, G.mixed, G.conv, G.hist, G.q, G.k, G.v); });
      tm("fx state+hist", [&] { E.state(G, M, M); });
      tm("fx gdn_out", [&] { fx::k_gdn_out(E.q, M, MR, E.core, E.z, G.gnorm, G.out.sg_in, E.eps, E.alpha, E.qx, E.us, E.Aw); });
      tm("fx mlp_post", [&] { fx::k_mlp_post(E.q, M, MR, fx::MlpPost{Y.m.gate.part, Y.m.up.part, Y.m.gate.tau, Y.m.up.tau, Y.m.gate.sg_out, Y.m.up.sg_out, Y.m.down.sg_in, E.S, E.us, E.alpha, E.qx, E.Aw, E.eps}); }); } }
  for (auto& n : order) std::printf("%-24s %8.3f ms\n", n.c_str(), T[n] / R * 1e3);
  // the whole layer as the engine runs it (no waits between kernels): median and minimum of 15 batches of 50 layers
  std::vector<double> v;
  for (int b = 0; b < 15; ++b) { E.embed(M); E.res_norm(nullptr, M, Y.ln1, E.first_sg(0), true); E.q.wait(); auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) { E.gdn(Y.g, M, M); E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); E.mlp(Y.m, M); E.res_norm(&Y.m.down, M, Y.ln1, E.first_sg(0), true); }
    E.q.wait(); v.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 50 * 1e3); }
  std::sort(v.begin(), v.end());
  std::printf("WHOLE LAYER (gdn + res_norm + mlp + res_norm), M = %d, %s path: median %.3f ms, min %.3f ms\n", M, E.fast ? "fast" : "scalar", v[7], v[0]);
}
