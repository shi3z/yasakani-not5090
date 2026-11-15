// Correctness and cost of float divide / sqrt on the device: raw, raw + backend option, fma-corrected.
#include "../src/kern.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
int main() {
  sycl::queue q{sycl::gpu_selector_v}; const size_t n = 1 << 24; std::mt19937_64 g(1);
  std::vector<float> a(n), b(n);
  for (size_t i = 0; i < n; ++i) { a[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 40) - 20); b[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 20) - 10); }
  float *da = sycl::malloc_device<float>(n, q), *db = sycl::malloc_device<float>(n, q), *dr = sycl::malloc_device<float>(n * 2, q);
  q.memcpy(da, a.data(), n * 4); q.memcpy(db, b.data(), n * 4); q.wait();
  std::vector<float> r(n * 2);
  auto check = [&](const char* nm, double ms) { q.memcpy(r.data(), dr, n * 8).wait(); size_t bd = 0, bs = 0;
    for (size_t i = 0; i < n; ++i) { bd += r[2 * i] != a[i] / b[i]; bs += r[2 * i + 1] != std::sqrt(b[i]); }
    std::printf("%-14s div mismatches %8zu  sqrt mismatches %8zu  of %zu   %.2f ms per launch\n", nm, bd, bs, n, ms); };
  auto time = [&](auto&& launch) { launch().wait(); auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 20; ++i) launch(); q.wait();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20 * 1e3; };
  double t = time([&] { return q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { dr[2 * i] = da[i] / db[i]; dr[2 * i + 1] = sycl::sqrt(db[i]); }); }); check("raw", t);
  t = time([&] { return q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { dr[2 * i] = yx::fdiv(da[i], db[i]); dr[2 * i + 1] = yx::fsqrt(db[i]); }); }); check("fma-corrected", t);
  t = time([&] { return q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { dr[2 * i] = da[i] * db[i]; dr[2 * i + 1] = db[i] + da[i]; }); }); std::printf("%-14s (mul and add only, for scale)                              %.2f ms per launch\n", "baseline", t);
}
