// SPDX-License-Identifier: Apache-2.0
// LM head: GEMM (4-bit matrix, no output rotation) -> logits -> arg-max (ties: smaller id) / host-side sampler.
// spec/B_layers.md section 8, spec/D_spec_api.md section 4.
#pragma once
#include "engine.hpp"
#include <algorithm>
#include <cmath>
namespace yx {
struct SmpParams { bool on = false; double temp = 0.6, top_p = 0.95, min_p = 0.0; int top_k = 20, version = 3; uint64_t seed = 0; };
struct Head {
  Tensor T; float* fnorm = nullptr; float* logits = nullptr; float* bv = nullptr; int* bi = nullptr;
  static constexpr int CH = 1024, NCH = (VOC + CH - 1) / CH;
  void init(Engine& E) {
    T = upload(E.q, E.C, "lm_head"); T.part = E.dev<int64_t>((size_t)MRMAX * VOC);
    fnorm = upload_vec(E.q, E.C, "norm", H); logits = E.dev<float>((size_t)MRMAX * VOC); bv = E.dev<float>(MRMAX * NCH); bi = E.dev<int>(MRMAX * NCH);
  }
  // logits of rows [row0, row0 + rows) of the pass (the last res_norm must have used fnorm and T.sg_in); arg-max per row into out
  void run(Engine& E, int M, int row0, int rows, int* out) {
    E.gemm_group({&T}, M);
    const int64_t* acc = T.part; const float *tau = T.tau, *usp = E.us, *SS = E.S; float *lg = logits, *pv = bv; int* pi = bi; const float e = E.eps;
    E.q.parallel_for(sycl::range<1>((size_t)rows * NCH), [=](sycl::id<1> it) { const int row = row0 + (int)(it[0] / NCH), c = (int)(it[0] % NCH);
      const float r = rfac(SS + row * SST, e), u = usp[row]; float best = -3.4028234663852886e38f; int bidx = 0x7fffffff;
      const int v1 = (c + 1) * CH < VOC ? (c + 1) * CH : VOC;
      for (int v = c * CH; v < v1; ++v) { const float x = (((float)acc[(size_t)row * VOC + v] * u) * r) * tau[v]; lg[(size_t)row * VOC + v] = x; if (x > best) { best = x; bidx = v; } }
      pv[row * NCH + c] = best; pi[row * NCH + c] = bidx; });
    std::vector<float> hv((size_t)MRMAX * NCH); std::vector<int> hi((size_t)MRMAX * NCH);
    E.q.memcpy(hv.data() + row0 * NCH, bv + row0 * NCH, (size_t)rows * NCH * 4); E.q.memcpy(hi.data() + row0 * NCH, bi + row0 * NCH, (size_t)rows * NCH * 4).wait();
    for (int r = 0; r < rows; ++r) { float best = -3.4028234663852886e38f; int b = 0x7fffffff;
      for (int c = 0; c < NCH; ++c) { const float x = hv[(row0 + r) * NCH + c]; if (x > best) { best = x; b = hi[(row0 + r) * NCH + c]; } }   // chunks ascend: first maximum = smallest id
      out[r] = b; }
  }
  void read_logits(Engine& E, int row, float* dst) { E.q.memcpy(dst, logits + (size_t)row * VOC, (size_t)VOC * 4).wait(); }
  // sampler (host, double): top-k -> top-p -> min-p -> Gumbel-max with noise hashed from (seed, position, id)
  static uint64_t mix(uint64_t z) { z += 0x9E3779B97F4A7C15ull; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
  static int sample(const float* lg, const SmpParams& P, int64_t pos) {
    const int k = P.top_k; std::vector<std::pair<float, int>> c; c.reserve(64);
    auto better = [](const std::pair<float, int>& a, const std::pair<float, int>& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); };
    for (int v = 0; v < VOC; ++v) { std::pair<float, int> x{lg[v], v};
      if ((int)c.size() < k) { c.push_back(x); std::push_heap(c.begin(), c.end(), better); }
      else if (better(x, c.front())) { std::pop_heap(c.begin(), c.end(), better); c.back() = x; std::push_heap(c.begin(), c.end(), better); } }
    std::sort(c.begin(), c.end(), better);
    double z[32], w[32], tot = 0; for (int j = 0; j < k; ++j) { z[j] = ((double)c[j].first - (double)c[0].first) / P.temp; w[j] = std::exp(z[j]); tot += w[j]; }
    int n = k; double run = 0; for (int j = 0; j < k; ++j) { run += w[j]; if (run >= P.top_p * tot) { n = j + 1; break; } }
    if (P.min_p > 0) { int m = 1; while (m < n && w[m] >= P.min_p) ++m; n = m; }
    int pick = n - 1;
    if (P.version == 3) { double best = -INFINITY;
      for (int j = 0; j < n; ++j) { const uint64_t h = mix(mix(mix(P.seed) ^ (uint64_t)pos) ^ ((uint64_t)(uint32_t)c[j].second * 0xD6E8FEB86659FD93ull));
        const double u = ((double)(h >> 11) + 0.5) * 1.1102230246251565e-16, g = z[j] - std::log(-std::log(u)); if (g > best) { best = g; pick = j; } } }
    else { double s = 0; for (int j = 0; j < n; ++j) s += w[j]; const double u = (double)(mix(mix(P.seed) ^ (uint64_t)pos) >> 11) * 1.1102230246251565e-16 * s; run = 0;
      for (int j = 0; j < n; ++j) { run += w[j]; if (u < run) { pick = j; break; } } }
    return c[pick].second;
  }
};
}  // namespace yx
