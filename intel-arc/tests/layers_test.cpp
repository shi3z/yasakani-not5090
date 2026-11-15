// Runs a prefill pass of M rows through the first GDN layers and dumps h after every layer.
#include "../src/engine.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
int main(int argc, char** argv) {
  const int upto = atoi(argv[1]);
  std::ifstream f("work/ids.bin", std::ios::binary | std::ios::ate); int M = (int)f.tellg() / 4; f.seekg(0); std::vector<int> ids(M); f.read((char*)ids.data(), M * 4);
  auto t0 = std::chrono::steady_clock::now();
  yx::Engine E(getenv("YX_MODEL") ? getenv("YX_MODEL") : "models/speed/qwen38_27b_speed.yskn", upto);
  std::printf("loaded %d layers in %.1f s, M = %d\n", upto, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), M);
  if (getenv("YX_M")) M = atoi(getenv("YX_M")); const int REPS = getenv("YX_REPS") ? atoi(getenv("YX_REPS")) : 2;
  E.q.memcpy(E.ids, ids.data(), M * 4).wait();
  std::vector<float> h((size_t)M * yx::H);
  for (int rep = 0; rep < REPS; ++rep) {
    auto t1 = std::chrono::steady_clock::now();
    if (rep) for (int l = 0; l < upto; ++l) if (!E.L[l].full) { E.q.memset(E.L[l].g.S, 0, 48 * 128 * 128 * 4); E.q.memset(E.L[l].g.hist, 0, 3 * yx::CV * 4); }
    E.embed(M); E.res_norm(nullptr, M, E.L[0].ln1, E.first_sg(0), true);
    for (int l = 0; l < upto; ++l) { yx::Layer& Y = E.L[l];
      E.gdn(Y.g, M, M); E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false);
      E.mlp(Y.m, M); E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, E.first_sg(l + 1), !E.L[l + 1].full);
      if (!rep) { E.q.wait(); E.q.memcpy(h.data(), E.h, h.size() * 4).wait(); std::ofstream o("work/xe_h_" + std::to_string(l) + ".bin", std::ios::binary); o.write((char*)h.data(), h.size() * 4); } }
    E.q.wait();
    std::printf("pass %d: %.1f ms for %d layers\n", rep, std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() * 1e3, upto);
  }
}
