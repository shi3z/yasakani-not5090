// Which float primitives on the device are bit-identical to the host (IEEE)?
#include "../src/kern.hpp"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
int main() {
  sycl::queue q{sycl::gpu_selector_v}; const size_t n = 1 << 20; std::mt19937_64 g(1);
  std::vector<float> a(n), b(n); std::vector<int64_t> L(n);
  for (size_t i = 0; i < n; ++i) { a[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 40) - 20) * ((g() & 1) ? 1 : -1); b[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 20) - 10);
    L[i] = (int64_t)(g() >> (11 + g() % 30)) * ((g() & 1) ? 1 : -1); }
  float *da = sycl::malloc_device<float>(n, q), *db = sycl::malloc_device<float>(n, q), *dr = sycl::malloc_device<float>(n * 10, q); int64_t* dl = sycl::malloc_device<int64_t>(n, q);
  q.memcpy(da, a.data(), n * 4); q.memcpy(db, b.data(), n * 4); q.memcpy(dl, L.data(), n * 8); q.wait();
  q.parallel_for(sycl::range<1>(n), [=](sycl::id<1> i) { const float x = da[i], y = db[i]; float* r = dr + i * 10;
    r[0] = yx::fdiv(x, 127.f); r[1] = yx::fdiv(x, y); r[2] = yx::fdiv(1.f, yx::fsqrt(y)); r[3] = yx::fsqrt(y); r[4] = sycl::rint(x); r[5] = (float)dl[i]; r[6] = yx::rf16(x); r[7] = yx::fdiv(16777216.f, y); r[8] = yx::pm_expf(x * 0.5f); r[9] = x * y + y; }).wait();
  std::vector<float> r(n * 10); q.memcpy(r.data(), dr, n * 40).wait();
  const char* nm[10] = {"x/127", "x/y", "1/sqrt(y)", "sqrt(y)", "rint", "(float)int64", "rf16", "2^24/y", "pm_expf", "x*y+y"}; size_t bad[10] = {};
  for (size_t i = 0; i < n; ++i) { const float x = a[i], y = b[i]; volatile float xy = x * y;
    float e[10] = {x / 127.f, x / y, 1.f / std::sqrt(y), std::sqrt(y), std::nearbyint(x), (float)L[i], 0, 16777216.f / y, 0, xy + y};
    for (int k = 0; k < 10; ++k) if (k != 6 && k != 8) bad[k] += std::memcmp(&e[k], &r[i * 10 + k], 4) != 0;
    { _Float16 hh = (_Float16)x; float e6 = (float)hh; bad[6] += std::memcmp(&e6, &r[i * 10 + 6], 4) != 0; } }
  for (int k = 0; k < 10; ++k) if (k != 8) std::printf("%-14s mismatches %zu / %zu\n", nm[k], bad[k], n);
}
