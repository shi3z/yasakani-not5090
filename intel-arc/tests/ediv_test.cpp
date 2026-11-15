// Are ESIMD float divide / sqrt / fma bit-identical to IEEE?
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace sycl::ext::intel::esimd;
int main() {
  sycl::queue q{sycl::gpu_selector_v}; const size_t n = 1 << 22; std::mt19937_64 g(1);
  std::vector<float> a(n), b(n);
  for (size_t i = 0; i < n; ++i) { a[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 40) - 20) * ((g() & 1) ? 1 : -1); b[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 20) - 10); }
  float *da = sycl::malloc_device<float>(n, q), *db = sycl::malloc_device<float>(n, q), *dr = sycl::malloc_device<float>(n * 4, q);
  q.memcpy(da, a.data(), n * 4); q.memcpy(db, b.data(), n * 4); q.wait();
  q.parallel_for(sycl::range<1>(n / 16), [=](sycl::id<1> i) SYCL_ESIMD_KERNEL {
    simd<float, 16> x = block_load<float, 16>(da + i * 16), y = block_load<float, 16>(db + i * 16);
    block_store<float, 16>(dr + i * 16, x / y);
    block_store<float, 16>(dr + n + i * 16, sqrt_ieee<float, 16>(y));
    block_store<float, 16>(dr + 2 * n + i * 16, div_ieee<float, 16>(x, y));
    simd<float, 16> r = rnde<float, 16>(x); block_store<float, 16>(dr + 3 * n + i * 16, r); }).wait();
  std::vector<float> r(n * 4); q.memcpy(r.data(), dr, n * 16).wait(); size_t bd = 0, bs = 0, bf = 0, br = 0;
  for (size_t i = 0; i < n; ++i) { bd += r[i] != a[i] / b[i]; bs += r[n + i] != std::sqrt(b[i]); bf += r[2 * n + i] != a[i] / b[i]; br += r[3 * n + i] != std::nearbyint(a[i]); }
  std::printf("ESIMD: div mismatches %zu, sqrt_ieee %zu, div_ieee %zu, rnde %zu of %zu\n", bd, bs, bf, br, n);
}
