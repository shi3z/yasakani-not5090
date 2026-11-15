#include "../src/gemm.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
template <class T> static std::vector<T> rd(const std::string& p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate); size_t n = f.tellg(); f.seekg(0); std::vector<T> v(n / sizeof(T)); f.read((char*)v.data(), n); return v; }
template <int BITS, int MR> static int run(sycl::queue& q, const std::string& p) {
  auto hdr = rd<int>(p + ".hdr"); const int N = hdr[0], Kc = hdr[1];
  auto words = rd<uint32_t>(p + ".words"); auto lut = rd<uint32_t>(p + ".lut"); auto qx = rd<int8_t>(p + ".qx"); auto aw = rd<int32_t>(p + ".aw"); auto ref = rd<int64_t>(p + ".ref");
  auto dv = [&](auto& v) { auto* d = sycl::malloc_device<typename std::remove_reference_t<decltype(v)>::value_type>(v.size(), q); q.memcpy(d, v.data(), v.size() * sizeof(v[0])).wait(); return d; };
  auto *d_w = dv(words); auto *d_l = dv(lut); auto *d_x = dv(qx); auto *d_a = dv(aw); int64_t* d_p = sycl::malloc_device<int64_t>(ref.size(), q);
  yx::gemm<BITS, MR>(q, d_w, d_l, d_x, d_a, d_p, N, Kc).wait();
  std::vector<int64_t> z(ref.size()); q.memcpy(z.data(), d_p, z.size() * 8).wait();
  size_t bad = 0; for (size_t i = 0; i < z.size(); ++i) bad += z[i] != ref[i];
  double best = 1e9;
  for (int rep = 0; rep < 5; ++rep) { auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 50; ++i) yx::gemm<BITS, MR>(q, d_w, d_l, d_x, d_a, d_p, N, Kc);
    q.wait(); best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 50); }
  std::printf("%-40s bits %d MR %2d: %.3f ms  %.0f Gw/s  mismatches %zu / %zu\n", p.c_str() + 20, BITS, MR, best * 1e3, (double)N * Kc / best / 1e9, bad, z.size());
  return bad != 0;
}
int main(int argc, char** argv) {
  sycl::queue q{sycl::gpu_selector_v};
  std::string p = std::string("/data/yasakani/tmp/t_") + argv[1]; const int bits = atoi(argv[2]), mr = atoi(argv[3]);
  if (bits == 6 && mr == 1) return run<6, 1>(q, p); if (bits == 6 && mr == 2) return run<6, 2>(q, p); if (bits == 6 && mr == 4) return run<6, 4>(q, p);
  if (bits == 6 && mr == 8) return run<6, 8>(q, p); if (bits == 6 && mr == 16) return run<6, 16>(q, p);
  if (bits == 8 && mr == 1) return run<8, 1>(q, p); if (bits == 8 && mr == 8) return run<8, 8>(q, p); if (bits == 8 && mr == 16) return run<8, 16>(q, p);
  return 2;
}
