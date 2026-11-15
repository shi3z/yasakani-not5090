// SLM / USM atomic updates with duplicate addresses inside one ESIMD vector: are all lanes counted?
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <cstdio>
using namespace sycl::ext::intel::esimd;
int main() {
  sycl::queue q(sycl::gpu_selector_v, sycl::property::queue::in_order());
  uint32_t* g = sycl::malloc_device<uint32_t>(4096, q); q.memset(g, 0, 4096 * 4).wait();
  uint32_t* g2 = sycl::malloc_device<uint32_t>(1024, q); q.memset(g2, 0, 1024 * 4).wait();
  const int NG = 8, LS = 10;
  q.parallel_for(sycl::nd_range<1>(NG * LS, LS), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<4096>();
    const uint32_t lid = it.get_local_id(0), grp = it.get_group(0);
    for (uint32_t b = lid; b < 16; b += LS) slm_block_store<uint32_t, 64>(b * 256, simd<uint32_t, 64>(0u));
    barrier();
    for (int r = 0; r < 100; ++r) {
      simd<uint32_t, 16> bin(0u); bin.template select<8, 1>(8) = simd<uint32_t, 8>(7u + lid);      // lanes 0..7 -> bin 0, lanes 8..15 -> bin 7 + lid
      simd_mask<16> m = simd<uint32_t, 16>(0, 1) != 3u;                                              // lane 3 masked off
      slm_atomic_update<atomic_op::inc, uint32_t, 16>(bin * 4u, m);
    }
    barrier();
    for (uint32_t b = lid; b < 16; b += LS) { simd<uint32_t, 64> v = slm_block_load<uint32_t, 64>(b * 256);
      block_store<uint32_t, 64>(g + grp * 0 + 1024 + b * 64 + grp * 0, v);                             // last group wins (all groups identical)
      for (int k = 0; k < 4; ++k) { simd<uint32_t, 16> x = v.template select<16, 1>(16 * k);
        atomic_update<atomic_op::add, uint32_t, 16>(g2, simd<uint32_t, 16>((b * 64 + 16 * k) * 4, 4), x, x != 0u); } }
  }).wait();
  std::vector<uint32_t> h(4096), h2(1024); q.memcpy(h.data(), g, 4096 * 4).wait(); q.memcpy(h2.data(), g2, 1024 * 4).wait();
  printf("slm bin0 = %u (expect %d), bin7 = %u (expect 800), bin16 = %u (expect 800)\n", h[1024], 7 * 100 * LS, h[1024 + 7], h[1024 + 16]);
  printf("global bin0 = %u (expect %d), bin7 = %u (expect %d)\n", h2[0], NG * 7 * 100 * LS, h2[7], NG * 800);
  return 0;
}
