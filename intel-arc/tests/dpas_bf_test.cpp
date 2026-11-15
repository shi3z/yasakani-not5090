// bf16 x bf16 -> float DPAS on Xe2: layout check.  acc[m][lane] += sum_k B[k][lane].lo * A[m][k].lo + B[k][lane].hi * A[m][k].hi
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
using namespace sycl::ext::intel::esimd;
static uint16_t f2bf(float f) { uint32_t u; memcpy(&u, &f, 4); u += 0x7fffu + ((u >> 16) & 1u); return (uint16_t)(u >> 16); }
static float bf2f(uint16_t h) { uint32_t u = (uint32_t)h << 16; float f; memcpy(&f, &u, 4); return f; }
int main() {
  sycl::queue q(sycl::gpu_selector_v, sycl::property::queue::in_order());
  constexpr int M = 4;
  std::vector<uint16_t> B(256), A(16 * M); std::vector<float> ref(16 * M, 0.f), out(16 * M);
  for (int i = 0; i < 256; ++i) B[i] = f2bf(std::sin(0.37f * i) * 3.f);
  for (int i = 0; i < 16 * M; ++i) A[i] = f2bf(std::cos(0.11f * i + 1.f));
  for (int m = 0; m < M; ++m) for (int l = 0; l < 16; ++l) { double s = 0.5; for (int k = 0; k < 8; ++k) for (int e = 0; e < 2; ++e) s += (double)bf2f(B[(k * 16 + l) * 2 + e]) * bf2f(A[(m * 8 + k) * 2 + e]); ref[m * 16 + l] = (float)s; }
  uint16_t* dB = sycl::malloc_device<uint16_t>(256, q); uint16_t* dA = sycl::malloc_device<uint16_t>(16 * M, q); float* dO = sycl::malloc_device<float>(16 * M, q);
  q.memcpy(dB, B.data(), 512).wait(); q.memcpy(dA, A.data(), 32 * M).wait();
  q.parallel_for(sycl::range<1>(1), [=](sycl::id<1>) SYCL_ESIMD_KERNEL {
    simd<int, 128> b; b.template select<64, 1>(0) = block_load<int, 64>((const int*)dB); b.template select<64, 1>(64) = block_load<int, 64>((const int*)dB + 64);
    simd<int, 8 * M> a = block_load<int, 8 * M>((const int*)dA); simd<float, 16 * M> acc(0.5f);
    acc = xmx::dpas<8, M, float, float, int, int, xmx::dpas_argument_type::bf16, xmx::dpas_argument_type::bf16>(acc, b, a);
    block_store<float, 16 * M>(dO, acc); }).wait();
  q.memcpy(out.data(), dO, 64 * M).wait(); double mx = 0;
  for (int i = 0; i < 16 * M; ++i) mx = std::fmax(mx, std::fabs(out[i] - ref[i]));
  printf("max abs diff %.3g (values ~ %.3f %.3f ref %.3f %.3f)\n", mx, out[0], out[17], ref[0], ref[17]);
  return 0;
}
