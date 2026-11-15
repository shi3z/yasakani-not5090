// Compressed GEMM for the B60 in SYCL ESIMD: per thread 16 rows, 3-bit trellis codes -> int8 via a table in SLM -> DPAS with M=8 input rows.
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <sycl/ext/intel/experimental/grf_size_properties.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
using namespace sycl;
using namespace sycl::ext::intel::esimd;
namespace xmx = sycl::ext::intel::esimd::xmx;
template <class T> static std::vector<T> rd(const std::string& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate); size_t n = f.tellg(); f.seekg(0);
  std::vector<T> v(n / sizeof(T)); f.read((char*)v.data(), n); return v; }
constexpr int BITS = 6, NW = 4 * BITS, M = 8;
int main(int argc, char** argv) {
  std::string p = std::string("work/") + argv[1];
  const int LS = argc > 2 ? atoi(argv[2]) : 16;
  auto hdr = rd<int>(p + ".hdr"); const int rows = hdr[0], cols = hdr[1], S = cols / 256;
  auto words = rd<uint32_t>(p + ".words"); auto lut = rd<uint32_t>(p + ".lut"); auto x = rd<int8_t>(p + ".x"); auto ref = rd<int>(p + ".ref");
  queue q{gpu_selector_v};
  std::printf("%s on %s\n", argv[1], q.get_device().get_info<sycl::info::device::name>().c_str());
  uint32_t* d_w = malloc_device<uint32_t>(words.size(), q); uint32_t* d_lut = malloc_device<uint32_t>(4096, q);
  int8_t* d_x = malloc_device<int8_t>(x.size(), q); int* d_z = malloc_device<int>((size_t)M * rows, q);
  q.memcpy(d_w, words.data(), words.size() * 4); q.memcpy(d_lut, lut.data(), 4096 * 4); q.memcpy(d_x, x.data(), x.size()); q.wait();
  const int nthreads = rows / 16, rows32 = rows / 32;
  auto launch = [&] {
    return q.parallel_for(nd_range<1>(nthreads, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
      slm_init<16384>();
      const uint32_t lid = it.get_local_id(0);
      for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(d_lut + i * 64));
      barrier();
      const uint32_t r0 = 16u * (uint32_t)it.get_global_id(0), lane0 = r0 & 31u, gr = r0 >> 5;
      simd<int, 16 * M> acc(0);
      for (int s = 0; s < S; ++s) {
        const uint32_t g = (uint32_t)s * rows32 + gr;
        simd<uint32_t, 16> w[NW + 1];
        for (int c = 0; c < BITS; ++c) {
          simd<uint32_t, 64> blk = block_load<uint32_t, 64>(d_w + ((g * BITS + c) * 32 + lane0) * 4);
          for (int k = 0; k < 4; ++k) w[c * 4 + k] = blk.select<16, 4>(k);
        }
        w[NW] = 0;
        for (int ch = 0; ch < 8; ++ch) {
          simd<int, 128> B;
          for (int i = 0; i < 8; ++i) {
            simd<uint32_t, 16> gth[2];
            for (int h = 0; h < 2; ++h) {
              const int j = 16 * ch + 2 * i + h, a = BITS * (j + 1) - 12;
              simd<uint32_t, 16> st;
              if (a < 0) st = (w[0] << (uint32_t)(-a)) & 0xfffu;
              else { const int wi = a >> 5, sh = a & 31;
                     st = sh + 12 <= 32 ? ((w[wi] >> (uint32_t)sh) & 0xfffu) : (((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0xfffu); }
              gth[h] = slm_gather<uint32_t, 16>(st * 4u);
            }
            B.select<16, 1>(i * 16) = simd<int, 16>((gth[0] & 0xffffu) | (gth[1] << 16u));
          }
          simd<int, 8 * M> A = block_load<int, 8 * M>((const int*)(d_x + (size_t)(s * 8 + ch) * M * 32));
          acc = xmx::dpas<8, M, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(acc, B, A);
        }
      }
      for (int m = 0; m < M; ++m) block_store<int, 16>(d_z + (size_t)m * rows + r0, acc.select<16, 1>(m * 16));
    });
  };
  launch().wait();
  std::vector<int> z((size_t)M * rows); q.memcpy(z.data(), d_z, z.size() * 4).wait();
  size_t bad = 0; for (size_t i = 0; i < z.size(); ++i) bad += z[i] != ref[i];
  double best = 1e9;
  for (int rep = 0; rep < 5; ++rep) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) launch();
    q.wait(); best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 100);
  }
  std::printf("  M=%d LS=%d: %.3f ms  %.1f Gw/s  mismatches %zu / %zu\n", M, LS, best * 1e3, (double)rows * cols / best / 1e9, bad, z.size());
}
