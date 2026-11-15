// SPDX-License-Identifier: Apache-2.0
// Shared helpers of the CUDA micro-benchmarks (mirror of b60/common.hpp).
#pragma once
#include <cuda_runtime.h>
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
inline double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Error handling.  soft() = true: print a "# error" comment line and let the caller skip the row instead of exiting.
inline bool& soft() { static bool s = false; return s; }
inline bool ck(cudaError_t e, const char* what) {
  if (e == cudaSuccess) return true;
  if (soft()) { std::printf("# error: %s: %s\n", what, cudaGetErrorString(e)); std::fflush(stdout); return false; }
  std::fprintf(stderr, "CUDA error: %s: %s\n", what, cudaGetErrorString(e)); std::exit(1);
}
inline bool sync() { return ck(cudaStreamSynchronize(0), "synchronize"); }      // all kernels run on the default stream
inline bool launched() { return ck(cudaGetLastError(), "launch"); }

// Best time per launch in seconds (-1 on a soft error).  queued: `inner` launches, one synchronize.  sync: synchronize after every launch.
template <class L> double best_time(L&& launch, bool sync_each = false, double budget = 0.04, int reps = 5) {
  launch(); if (!launched() || !sync()) return -1;
  double t0 = now(); launch(); if (!sync()) return -1; const double one = now() - t0;
  const int inner = (int)std::clamp(budget / std::max(one, 1e-6), 1.0, 400.0);
  double best = 1e30;
  for (int r = 0; r < reps; ++r) {
    t0 = now();
    for (int i = 0; i < inner; ++i) { launch(); if (sync_each) cudaStreamSynchronize(0); }
    if (!sync()) return -1;
    best = std::min(best, (now() - t0) / inner);
  }
  return launched() ? best : -1;
}
inline bool want(const char* name) { const char* f = getenv("ARCH_ONLY"); return !f || std::strstr(f, name); }   // ARCH_ONLY=a,b selects sections
inline uint32_t rng(uint64_t& s) { s = s * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(s >> 32); }

inline const cudaDeviceProp& prop() {
  static cudaDeviceProp p; static bool init = false;
  if (!init) { int dev = 0; ck(cudaGetDevice(&dev), "cudaGetDevice"); ck(cudaGetDeviceProperties(&p, dev), "cudaGetDeviceProperties"); init = true; }
  return p;
}
inline void banner(const char* what) {
  const cudaDeviceProp& p = prop();
  std::printf("# %s on %s\n", what, p.name);
  std::printf("# sm_%d%d, %d SMs, max %d threads/SM, %d registers/SM, shared memory %zu B/SM, %zu B/block (opt-in %zu B)\n", p.major, p.minor,
              p.multiProcessorCount, p.maxThreadsPerMultiProcessor, p.regsPerMultiprocessor, (size_t)p.sharedMemPerMultiprocessor,
              (size_t)p.sharedMemPerBlock, (size_t)p.sharedMemPerBlockOptin);
}

template <class T> T* dalloc(size_t n) { void* p = nullptr; ck(cudaMalloc(&p, std::max<size_t>(n, 1) * sizeof(T)), "cudaMalloc"); return (T*)p; }
template <class T> T* upload(const std::vector<T>& h) { T* d = dalloc<T>(h.size()); ck(cudaMemcpy(d, h.data(), h.size() * sizeof(T), cudaMemcpyHostToDevice), "upload"); return d; }
template <class T> std::vector<T> download(const void* d, size_t n) { std::vector<T> h(n); ck(cudaMemcpy(h.data(), d, n * sizeof(T), cudaMemcpyDeviceToHost), "download"); return h; }
}  // namespace ab
