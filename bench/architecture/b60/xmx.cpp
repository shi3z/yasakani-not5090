// SPDX-License-Identifier: Apache-2.0
// XMX (dpas) throughput and latency on the B60.
//  reg : register-only loops.  D = rows per dpas call (repeat count), NCH = number of independent accumulators.
//        NCH = 1 is a dependent chain (latency), NCH = 4 independent streams (throughput).  Swept over the number of threads.
//        One dpas "row" = 16 lanes x depth 8 x (4 int8 or 2 bf16 products) = 512 / 256 multiply-accumulates.
//  gemm: uncompressed int8 (or bf16) GEMM.  W [N/16][K/32][8 dwords][16 lanes] (bf16: K/16), X [K/32][M][8 dwords], Y [M][N] int32 / float.
//        thread = 16 output rows x D*ND input rows; M = RS * D * ND.
// usage: xmx      env ARCH_ONLY=reg,gemm   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
using AT = xmx::dpas_argument_type;

template <int D, bool BF> using acc_t = simd<std::conditional_t<BF, float, int>, 16 * D>;
template <int D, bool BF> inline __attribute__((always_inline)) acc_t<D, BF> dp(acc_t<D, BF> a, simd<int, 128> B, simd<int, 8 * D> A) {
  if constexpr (BF) return xmx::dpas<8, D, float, float, int, int, AT::bf16, AT::bf16>(a, B, A);
  else return xmx::dpas<8, D, int, int, int, int, AT::s8, AT::s8>(a, B, A);
}

template <int D, int NCH, bool BF>
event k_reg(queue& q, const int* src, float* out, int threads, int iters) {
  return q.parallel_for(range<1>(threads), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<int, 128> B; B.template select<64, 1>(0) = block_load<int, 64>(src); B.template select<64, 1>(64) = block_load<int, 64>(src + 64);
    const simd<int, 8 * D> A = block_load<int, 8 * D>(src + 128);
    acc_t<D, BF> acc[NCH];
    unroll<NCH>([&](auto c) { acc[decltype(c)::value] = 0; });
    for (int it = 0; it < iters; ++it) unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; acc[k] = dp<D, BF>(acc[k], B, A); });
    simd<float, 16> s = 0;
    unroll<NCH>([&](auto c) { unroll<D>([&](auto m) { s += simd<float, 16>(acc[decltype(c)::value].template select<16, 1>(16 * decltype(m)::value)); }); });
    block_store<float, 16>(out + i[0] * 16, s); });
}
// Same loop shape with a 32-bit integer add instead of the dpas: calibrates the loop overhead and the cycle time.
template <int NCH>
event k_add(queue& q, const int* src, float* out, int threads, int iters) {
  return q.parallel_for(range<1>(threads), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const simd<int, 16> B = block_load<int, 16>(src);
    simd<int, 16> acc[NCH];
    unroll<NCH>([&](auto c) { acc[decltype(c)::value] = 0; });
    for (int it = 0; it < iters; ++it) unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; acc[k] = acc[k] + B; });
    simd<float, 16> s = 0;
    unroll<NCH>([&](auto c) { s += simd<float, 16>(acc[decltype(c)::value]); });
    block_store<float, 16>(out + i[0] * 16, s); });
}

template <int D, int ND, bool BF>
event k_gemm(queue& q, const int* W, const int* X, void* Y, int N, int nchunk, int M) {
  const int RS = M / (D * ND);
  return q.parallel_for(range<1>((size_t)(N / 16) * RS), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const uint32_t gid = (uint32_t)i[0], nrt = (uint32_t)N / 16u, rs = gid / nrt, rt = gid - rs * nrt;
    const int m0 = (int)rs * D * ND;
    acc_t<D, BF> acc[ND];
    unroll<ND>([&](auto c) { acc[decltype(c)::value] = 0; });
    const int* w = W + (size_t)rt * nchunk * 128;
    for (int c = 0; c < nchunk; ++c) {
      simd<int, 128> B; B.template select<64, 1>(0) = block_load<int, 64>(w + c * 128); B.template select<64, 1>(64) = block_load<int, 64>(w + c * 128 + 64);
      const int* xa = X + ((size_t)c * M + m0) * 8;
      unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
        const simd<int, 8 * D> A = block_load<int, 8 * D>(xa + dd * D * 8);
        acc[dd] = dp<D, BF>(acc[dd], B, A); });
    }
    unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value; unroll<D>([&](auto mi) { constexpr int m = decltype(mi)::value;
      using T = std::conditional_t<BF, float, int>;
      block_store<T, 16>((T*)Y + (size_t)(m0 + dd * D + m) * N + rt * 16, acc[dd].template select<16, 1>(16 * m)); }); });
  });
}

// Prefill-style GEMM: a work-group of LS threads owns 16 output rows.  It copies their weights (nchunk x 512 bytes, at most
// 128 KiB) from global memory to SLM ONCE; thread lid then handles the 8*ND input rows starting at lid*8*ND, reading the weight
// operand from SLM.  M = LS * 8 * ND input rows per launch, the weights cross the memory bus once.
template <int ND, int SLMB>
event k_gemm_slm(queue& q, const int* W, const int* X, int* Y, int N, int nchunk, int LS) {
  const int M = LS * 8 * ND;
  return q.parallel_for(nd_range<1>((size_t)(N / 16) * LS, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<SLMB>();
    const uint32_t lid = it.get_local_id(0), rt = (uint32_t)it.get_group(0);
    const int* w = W + (size_t)rt * nchunk * 128;
    for (int c = lid; c < nchunk; c += LS) { slm_block_store<int, 64>(c * 512, block_load<int, 64>(w + c * 128)); slm_block_store<int, 64>(c * 512 + 256, block_load<int, 64>(w + c * 128 + 64)); }
    barrier();
    const int m0 = (int)lid * 8 * ND;
    simd<int, 128> acc[ND];
    unroll<ND>([&](auto c) { acc[decltype(c)::value] = 0; });
    for (int c = 0; c < nchunk; ++c) {
      simd<int, 128> B; B.template select<64, 1>(0) = slm_block_load<int, 64>(c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(c * 512 + 256);
      const int* xa = X + ((size_t)c * M + m0) * 8;
      unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
        const simd<int, 64> A = block_load<int, 64>(xa + dd * 64);
        acc[dd] = dp<8, false>(acc[dd], B, A); });
    }
    unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value; unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value;
      block_store<int, 16>(Y + (size_t)(m0 + dd * 8 + m) * N + rt * 16, acc[dd].template select<16, 1>(16 * m)); }); });
  });
}

static uint16_t f2bf(float f) { uint32_t u; memcpy(&u, &f, 4); u += 0x7fffu + ((u >> 16) & 1u); return (uint16_t)(u >> 16); }

int main() {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "xmx");
  uint64_t seed = 99;
  if (ab::want("reg")) {
    std::vector<int> hs(256); for (auto& v : hs) v = (int)(ab::rng(seed) & 0x03030303u);          // small int8 values
    std::vector<uint16_t> hb(512); for (auto& v : hb) v = f2bf((float)(ab::rng(seed) >> 24) / 4096.f);
    int* src = malloc_device<int>(256, q); int* srcb = malloc_device<int>(256, q); float* out = malloc_device<float>(16 * 65536, q);
    q.memcpy(src, hs.data(), 1024); q.memcpy(srcb, hb.data(), 1024); q.wait();
    std::printf("section,kernel,rows_per_dpas,chains,threads,iters,ms,ns_per_dpas_row_in_thread,dpas_rows_per_s,TOPS_or_TFLOPS\n");
    const int thr[] = {1, 8, 32, 64, 128, 160, 320, 480, 640, 800, 960, 1280, 1600, 1920, 2560, 5120, 10240};
    auto run = [&](const char* name, int D, int NCH, int iters, double macs, auto&& launch) {
      for (int t : thr) { const double s = ab::best_time(q, [&] { return launch(t, iters); }, true, 0.03, 4);
        const double rows = (double)t * iters * NCH * D;
        std::printf("reg,%s,%d,%d,%d,%d,%.4f,%.3f,%.4g,%.2f\n", name, D, NCH, t, iters, s * 1e3, s * 1e9 / ((double)iters * NCH * D), rows / s, rows / s * macs * 2 / 1e12);
        std::fflush(stdout); } };
    run("add32", 1, 1, 200000, 16, [&](int t, int it) { return k_add<1>(q, src, out, t, it); });
    run("add32", 1, 4, 100000, 16, [&](int t, int it) { return k_add<4>(q, src, out, t, it); });
    run("dpas_s8", 1, 1, 100000, 512, [&](int t, int it) { return k_reg<1, 1, false>(q, src, out, t, it); });
    run("dpas_s8", 1, 4, 50000, 512, [&](int t, int it) { return k_reg<1, 4, false>(q, src, out, t, it); });
    run("dpas_s8", 8, 1, 20000, 512, [&](int t, int it) { return k_reg<8, 1, false>(q, src, out, t, it); });
    run("dpas_s8", 8, 4, 10000, 512, [&](int t, int it) { return k_reg<8, 4, false>(q, src, out, t, it); });
    run("dpas_bf16", 1, 1, 100000, 256, [&](int t, int it) { return k_reg<1, 1, true>(q, srcb, out, t, it); });
    run("dpas_bf16", 1, 4, 50000, 256, [&](int t, int it) { return k_reg<1, 4, true>(q, srcb, out, t, it); });
    run("dpas_bf16", 8, 1, 20000, 256, [&](int t, int it) { return k_reg<8, 1, true>(q, srcb, out, t, it); });
    run("dpas_bf16", 8, 4, 10000, 256, [&](int t, int it) { return k_reg<8, 4, true>(q, srcb, out, t, it); });
    free(src, q); free(srcb, q); free(out, q);
  }
  if (ab::want("gemm")) {
    std::printf("section,type,N,K,rows_M,rows_per_thread,threads,ms_queued,ms_sync,TOPS_or_TFLOPS,weight_GB_per_s,check\n");
    const int shapes[][2] = {{17408, 5120}, {5120, 17408}, {20480, 512}, {81920, 512}};
    for (auto& sh : shapes) for (int bf = 0; bf < 2; ++bf) {
      const int N = sh[0], K = sh[1], cols = bf ? 16 : 32, nchunk = K / cols, MAXM = (N == 17408 && !bf) ? 1024 : 64;
      const size_t nw = (size_t)(N / 16) * nchunk * 128, nx = (size_t)nchunk * MAXM * 8;
      std::vector<int> hw(nw), hx(nx);
      if (bf) { for (auto& v : hw) { const uint32_t r = ab::rng(seed); v = (int)(f2bf((float)(r & 255) / 256.f - 0.5f) | ((uint32_t)f2bf((float)(r >> 24) / 256.f - 0.5f) << 16)); }
                for (auto& v : hx) { const uint32_t r = ab::rng(seed); v = (int)(f2bf((float)(r & 255) / 256.f - 0.5f) | ((uint32_t)f2bf((float)(r >> 24) / 256.f - 0.5f) << 16)); } }
      else { for (auto& v : hw) v = (int)ab::rng(seed); for (auto& v : hx) v = (int)ab::rng(seed); }
      int* W = malloc_device<int>(nw, q); int* X = malloc_device<int>(nx, q); int* Y = malloc_device<int>((size_t)MAXM * N, q);
      q.memcpy(W, hw.data(), nw * 4); q.memcpy(X, hx.data(), nx * 4); q.wait();
      auto run = [&]<int D, int ND>(int M) {
        auto launch = [&] { return k_gemm<D, ND, (bool)0>(q, W, X, Y, N, nchunk, M); };
        auto launchb = [&] { return k_gemm<D, ND, (bool)1>(q, W, X, Y, N, nchunk, M); };
        const char* chk = "-";
        if (!bf && D * ND == 8 && M == 8) {            // exact check of the first 16 output rows
          launch().wait(); std::vector<int> y((size_t)M * N); q.memcpy(y.data(), Y, y.size() * 4).wait(); chk = "exact";
          for (int m = 0; m < M; ++m) for (int l = 0; l < 16; ++l) { int s = 0;
            for (int c = 0; c < nchunk; ++c) for (int k = 0; k < 8; ++k) { const uint32_t b = (uint32_t)hw[((size_t)c * 8 + k) * 16 + l], a = (uint32_t)hx[((size_t)c * M + m) * 8 + k];
              for (int e = 0; e < 4; ++e) s += (int)(int8_t)(b >> (8 * e)) * (int)(int8_t)(a >> (8 * e)); }
            if (s != y[(size_t)m * N + l]) chk = "MISMATCH"; } }
        const double tq = bf ? ab::best_time(q, launchb, false) : ab::best_time(q, launch, false);
        const double ts = bf ? ab::best_time(q, launchb, true) : ab::best_time(q, launch, true);
        std::printf("gemm,%s,%d,%d,%d,%d,%d,%.4f,%.4f,%.2f,%.1f,%s\n", bf ? "bf16" : "s8", N, K, M, D * ND, N / 16 * (M / (D * ND)), tq * 1e3, ts * 1e3,
                    2.0 * N * K * M / tq / 1e12, (double)nw * 4 / tq / 1e9, chk);
        std::fflush(stdout); };
      run.template operator()<1, 1>(1); run.template operator()<4, 1>(4); run.template operator()<8, 1>(8); run.template operator()<8, 2>(16);
      run.template operator()<8, 4>(32); run.template operator()<8, 2>(32);
      run.template operator()<8, 8>(64); run.template operator()<8, 4>(64); run.template operator()<8, 2>(64); run.template operator()<8, 1>(64);
      if (N == 17408 && !bf) {        // many input rows (prompt reading): weights re-read per row set vs staged once in SLM
        auto big = [&]<int ND>(int M, const char* how, auto&& launch) {
          launch().wait(); std::vector<int> y((size_t)16 * 2); const char* chk = "exact";      // check rows 0 and M-1, first 16 outputs
          for (int m : {0, M - 1}) { q.memcpy(y.data(), Y + (size_t)m * N, 64).wait();
            for (int l = 0; l < 16; ++l) { int sacc = 0;
              for (int c = 0; c < nchunk; ++c) for (int k = 0; k < 8; ++k) { const uint32_t b = (uint32_t)hw[((size_t)c * 8 + k) * 16 + l], a = (uint32_t)hx[((size_t)c * M + m) * 8 + k];
                for (int e = 0; e < 4; ++e) sacc += (int)(int8_t)(b >> (8 * e)) * (int)(int8_t)(a >> (8 * e)); }
              if (sacc != y[l]) chk = "MISMATCH"; } }
          const double tq = ab::best_time(q, launch, false), ts = ab::best_time(q, launch, true);
          std::printf("gemm,%s,%d,%d,%d,%d,%d,%.4f,%.4f,%.2f,%.1f,%s\n", how, N, K, M, 8 * ND, N / 16 * (M / (8 * ND)), tq * 1e3, ts * 1e3,
                      2.0 * N * K * M / tq / 1e12, (double)nw * 4 / tq / 1e9, chk); std::fflush(stdout); };
        for (int M : {256, 1024}) {
          big.template operator()<4>(M, "s8 weights re-read per 32 rows", [&] { return k_gemm<8, 4, false>(q, W, X, Y, N, nchunk, M); });
          big.template operator()<4>(M, "s8 weights staged in SLM (32 rows/thread)", [&] { return k_gemm_slm<4, 81920>(q, W, X, Y, N, nchunk, M / 32); });
          big.template operator()<2>(M, "s8 weights staged in SLM (16 rows/thread)", [&] { return k_gemm_slm<2, 81920>(q, W, X, Y, N, nchunk, M / 16); });
        }
      }
      free(W, q); free(X, q); free(Y, q);
    }
  }
  return 0;
}
