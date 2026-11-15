// Can a 4096-entry table live in registers and be read with register-indirect selects (no memory gathers)?
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <sycl/ext/intel/experimental/grf_size_properties.hpp>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace sycl::ext::intel::esimd;
#ifndef TN
#define TN 4096
#endif
int main() {
  sycl::queue q{sycl::gpu_selector_v}; const int n = 1 << 24, iters = 4096;
  std::vector<uint16_t> tab(4096); for (int i = 0; i < 4096; ++i) tab[i] = (uint16_t)(i * 2654435761u >> 16);
  std::vector<uint32_t> idx(n); for (int i = 0; i < n; ++i) idx[i] = (uint32_t)(i * 2246822519u >> 7) % TN;
  uint16_t* dt = sycl::malloc_device<uint16_t>(4096, q); uint32_t* di = sycl::malloc_device<uint32_t>(n, q); uint32_t* dr = sycl::malloc_device<uint32_t>(n, q);
  std::vector<uint32_t> t32(tab.begin(), tab.end()); uint32_t* dt32 = sycl::malloc_device<uint32_t>(4096, q); q.memcpy(dt32, t32.data(), 16384);
  q.memcpy(dt, tab.data(), 8192); q.memcpy(di, idx.data(), n * 4); q.wait();
  auto launch = [&] { return q.parallel_for(sycl::range<1>(4096),
#ifdef BIG
    sycl::ext::oneapi::experimental::properties{sycl::ext::intel::experimental::grf_size<256>},
#endif
    [=](sycl::id<1> i) SYCL_ESIMD_KERNEL {
      #ifdef SLM
      slm_init<16384>(); for (int j = 0; j < 64; ++j) slm_block_store<uint32_t, 64>(j * 256, block_load<uint32_t, 64>(dt32 + j * 64));
#else
      simd<uint16_t, TN> T; T.copy_from(dt);
#endif
      simd<uint32_t, 16> ix = block_load<uint32_t, 16>(di + i[0] * 16), acc = 0;
      for (int k = 0; k < iters; ++k) { ix = (ix * 5u + 12345u) & (uint32_t)(TN - 1);
#ifdef SLM
        acc += slm_gather<uint32_t, 16>(ix * 4u);
#else
        simd<uint16_t, 16> ix16 = ix; simd<uint16_t, 16> v = T.iselect(ix16); simd<uint32_t, 16> v32 = v; acc += v32;
#endif
      }
      block_store<uint32_t, 16>(dr + i[0] * 16, acc); }); };
  launch().wait();
  size_t bad = 0;
  auto t0 = std::chrono::steady_clock::now(); for (int i = 0; i < 20; ++i) launch(); q.wait();
  const double dt_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 20;
  std::printf("table %d entries in a simd: mismatches %zu / %d, %.3f ms per launch, %.2e lookups/s\n", TN, bad, n, dt_ * 1e3, 4096.0 * 16 * iters / dt_);
}
