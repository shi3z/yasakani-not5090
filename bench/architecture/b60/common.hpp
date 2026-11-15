// SPDX-License-Identifier: Apache-2.0
// Shared helpers of the B60 (SYCL ESIMD) micro-benchmarks.
#pragma once
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
namespace ab {
namespace es = sycl::ext::intel::esimd;
// Compile-time loop: ESIMD keeps arrays of simd on the stack when the loop is a run-time loop.
template <int N, class F> inline __attribute__((always_inline)) void unroll(F&& f) {
  [&]<size_t... I>(std::index_sequence<I...>) __attribute__((always_inline)) { (f(std::integral_constant<int, (int)I>{}), ...); }(std::make_index_sequence<N>{}); }

inline int intel_gpu(const sycl::device& d) { return d.is_gpu() && d.get_info<sycl::info::device::vendor_id>() == 0x8086 ? 1 : -1; }
inline sycl::queue make_queue() { return sycl::queue(intel_gpu, sycl::property::queue::in_order()); }

// ARCH_COMPILE_ONLY=1: build every kernel of the program (fills the persistent cache, runs nothing on the GPU) and exit.
inline bool compile_only(sycl::queue& q) {
  if (!getenv("ARCH_COMPILE_ONLY")) return false;
  auto t0 = std::chrono::steady_clock::now();
  auto b = sycl::get_kernel_bundle<sycl::bundle_state::input>(q.get_context(), {q.get_device()});
  sycl::build(b);
  std::fprintf(stderr, "compiled all kernels in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  return true;
}
inline double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// Best time per launch in seconds.  queued: `inner` launches, one wait.  sync: wait after every launch.
template <class L> double best_time(sycl::queue& q, L&& launch, bool sync = false, double budget = 0.04, int reps = 5) {
  launch(); q.wait();
  double t0 = now(); launch(); q.wait(); double one = now() - t0;
  int inner = (int)std::clamp(budget / std::max(one, 1e-6), 1.0, 400.0);
  double best = 1e30;
  for (int r = 0; r < reps; ++r) {
    t0 = now();
    for (int i = 0; i < inner; ++i) { launch(); if (sync) q.wait(); }
    q.wait(); best = std::min(best, (now() - t0) / inner);
  }
  return best;
}
inline bool want(const char* name) { const char* f = getenv("ARCH_ONLY"); return !f || std::strstr(f, name); }   // ARCH_ONLY=a,b selects sections
inline uint32_t rng(uint64_t& s) { s = s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s >> 32); }
inline void banner(sycl::queue& q, const char* what) {
  std::printf("# %s on %s\n", what, q.get_device().get_info<sycl::info::device::name>().c_str()); }
}  // namespace ab
