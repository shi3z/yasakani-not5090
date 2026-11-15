// SPDX-License-Identifier: Apache-2.0
// Compressed matrix product for Intel Xe2 (SYCL ESIMD + XMX).  Math: spec/A_gemv.md section 8.
//   acc[m][n] = sum over 512-column groups g of  A[m][g] * sum_{c in g} w8(n, c) * qx[m][c]        (exact int64)
// Weights: .yskn "warp" stream as stored in the file, decoded per lane through the 8-bit value table kept in SLM.
// Layouts: qx  = [Kc/32][MR][32] int8 (the MR input rows side by side for every 32 columns)
//          Aw  = [MR][G] int32,   part = [MR][N] int64,   lut = uint32[4096] (low byte = first weight, next byte = second)
#pragma once
#include <sycl/sycl.hpp>
#include <sycl/ext/intel/esimd.hpp>
#include <cstdint>
#include <utility>
namespace yx {
namespace es = sycl::ext::intel::esimd;
template <int N, class F> inline __attribute__((always_inline)) void unroll(F&& f) {
  [&]<size_t... I>(std::index_sequence<I...>) __attribute__((always_inline)) { (f(std::integral_constant<int, (int)I>{}), ...); }(std::make_index_sequence<N>{}); }

constexpr int GEMM_LS = 32;   // ESIMD threads per work-group (they share one copy of the value table)

// BITS: bits per step (6 or 8).  MR: input rows per launch (1, 2, 4, 8 or 16).
template <int BITS, int MR>
sycl::event gemm(sycl::queue& q, const uint32_t* stream, const uint32_t* lut, const int8_t* qx, const int32_t* Aw, int64_t* part,
                 int N, int Kc, const std::vector<sycl::event>& deps = {}) {
  constexpr int NW = 4 * BITS, D = MR > 8 ? 8 : MR, ND = MR > 8 ? 2 : 1;
  const int G = Kc / 512, rows32 = N / 32;
  return q.parallel_for(sycl::nd_range<1>(N / 16, GEMM_LS), deps, [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    using namespace es;
    slm_init<16384>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += GEMM_LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t r0 = 16u * (uint32_t)it.get_global_id(0), lane0 = r0 & 31u, gr = r0 >> 5;
    simd<int64_t, 16> acc[MR];
    unroll<MR>([&](auto mi) { acc[decltype(mi)::value] = 0; });
    for (int g = 0; g < G; ++g) {
      simd<int, 16 * D> d[ND];
      unroll<ND>([&](auto di) { d[decltype(di)::value] = 0; });
      for (int h = 0; h < 2; ++h) {
        const int s = 2 * g + h;
        const uint32_t gq = (uint32_t)s * rows32 + gr;
        simd<uint32_t, 16> w[NW + 1];
        unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
          simd<uint32_t, 64> blk = block_load<uint32_t, 64>(stream + ((size_t)(gq * BITS + c) * 32 + lane0) * 4);
          unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = blk.template select<16, 4>(k); });
        });
        unroll<8>([&](auto chi) { constexpr int ch = decltype(chi)::value;
          simd<int, 128> B;
          unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
            simd<uint32_t, 16> gth[2];
            unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value;
              constexpr int j = 16 * ch + 2 * i + hh, a = BITS * (j + 1) - 12;
              simd<uint32_t, 16> st;
              if constexpr (a < 0) st = (w[0] << (uint32_t)(-a)) & 0xfffu;
              else { constexpr int wi = a >> 5, sh = a & 31;
                     if constexpr (sh + 12 <= 32) st = (w[wi] >> (uint32_t)sh) & 0xfffu;
                     else st = ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0xfffu; }
              gth[hh] = slm_gather<uint32_t, 16>(st * 4u);
            });
            B.template select<16, 1>(i * 16) = simd<int, 16>((gth[0] & 0xffffu) | (gth[1] << 16u));
          });
          const int8_t* xa = qx + (size_t)(s * 8 + ch) * MR * 32;
          unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
            simd<int, 8 * D> A = block_load<int, 8 * D>((const int*)(xa + dd * 8 * 32));
            d[dd] = xmx::dpas<8, D, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(d[dd], B, A);
          });
        });
      }
      unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
        simd<int, 16> dm = d[m / 8].template select<16, 1>((m % 8) * 16);
        simd<int64_t, 16> wide = dm;
        acc[m] += wide * (int64_t)Aw[m * G + g];
      });
    }
    unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value; block_store<int64_t, 16>(part + (size_t)m * N + r0, acc[m]); });
  });
}

// Several matrices that share one input (same Kc, same qx / Aw, same value table) in ONE launch, optionally with the column
// groups of each matrix split over W workers (more, shorter threads: a single launch of a matrix with few rows does not fill
// the GPU otherwise).  Worker w handles groups [w*G/W, (w+1)*G/W) and writes its exact partial sums to
// part[(w*MR + m)*N + n]; with W == 1 that is the final result, otherwise the caller adds the W partials (exact int64).
struct GemmT { const uint32_t* stream = nullptr; int64_t* part = nullptr; int N = 0; };
template <int BITS, int MR>
sycl::event gemm_multi(sycl::queue& q, const GemmT* t, int nt, const uint32_t* lut, const int8_t* qx, const int32_t* Aw, int Kc, int W) {
  constexpr int NW = 4 * BITS, D = MR > 8 ? 8 : MR, ND = MR > 8 ? 2 : 1;
  const int G = Kc / 512;
  const uint32_t *s0 = t[0].stream, *s1 = nt > 1 ? t[1].stream : nullptr, *s2 = nt > 2 ? t[2].stream : nullptr;
  int64_t *p0 = t[0].part, *p1 = nt > 1 ? t[1].part : nullptr, *p2 = nt > 2 ? t[2].part : nullptr;
  const int N0 = t[0].N, N1 = nt > 1 ? t[1].N : 0, N2 = nt > 2 ? t[2].N : 0;
  const uint32_t o1 = (uint32_t)(N0 / 16) * W, o2 = o1 + (uint32_t)(N1 / 16) * W, total = o2 + (uint32_t)(N2 / 16) * W;
  return q.parallel_for(sycl::nd_range<1>(total, GEMM_LS), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    using namespace es;
    slm_init<16384>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += GEMM_LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t gid = (uint32_t)it.get_global_id(0);
    const int k = gid >= o2 ? 2 : (gid >= o1 ? 1 : 0);
    const uint32_t* stream = k == 2 ? s2 : (k == 1 ? s1 : s0); int64_t* part = k == 2 ? p2 : (k == 1 ? p1 : p0);
    const int N = k == 2 ? N2 : (k == 1 ? N1 : N0);
    const uint32_t loc = gid - (k == 2 ? o2 : (k == 1 ? o1 : 0u)), nrt = (uint32_t)N / 16u, wk = loc / nrt, rt = loc - wk * nrt;
    const int rows32 = N / 32, g0 = (int)(wk * (uint32_t)G) / W, g1 = (int)((wk + 1u) * (uint32_t)G) / W;
    const uint32_t r0 = 16u * rt, lane0 = r0 & 31u, gr = r0 >> 5;
    simd<int64_t, 16> acc[MR];
    unroll<MR>([&](auto mi) { acc[decltype(mi)::value] = 0; });
    for (int g = g0; g < g1; ++g) {
      simd<int, 16 * D> d[ND];
      unroll<ND>([&](auto di) { d[decltype(di)::value] = 0; });
      for (int h = 0; h < 2; ++h) {
        const int s = 2 * g + h;
        const uint32_t gq = (uint32_t)s * rows32 + gr;
        simd<uint32_t, 16> w[NW + 1];
        unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
          simd<uint32_t, 64> blk = block_load<uint32_t, 64>(stream + ((size_t)(gq * BITS + c) * 32 + lane0) * 4);
          unroll<4>([&](auto ki) { constexpr int kk = decltype(ki)::value; w[c * 4 + kk] = blk.template select<16, 4>(kk); });
        });
        unroll<8>([&](auto chi) { constexpr int ch = decltype(chi)::value;
          simd<int, 128> B;
          unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
            simd<uint32_t, 16> gth[2];
            unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value;
              constexpr int j = 16 * ch + 2 * i + hh, a = BITS * (j + 1) - 12;
              simd<uint32_t, 16> st;
              if constexpr (a < 0) st = (w[0] << (uint32_t)(-a)) & 0xfffu;
              else { constexpr int wi = a >> 5, sh = a & 31;
                     if constexpr (sh + 12 <= 32) st = (w[wi] >> (uint32_t)sh) & 0xfffu;
                     else st = ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0xfffu; }
              gth[hh] = slm_gather<uint32_t, 16>(st * 4u);
            });
            B.template select<16, 1>(i * 16) = simd<int, 16>((gth[0] & 0xffffu) | (gth[1] << 16u));
          });
          const int8_t* xa = qx + (size_t)(s * 8 + ch) * MR * 32;
          unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
            simd<int, 8 * D> A = block_load<int, 8 * D>((const int*)(xa + dd * 8 * 32));
            d[dd] = xmx::dpas<8, D, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(d[dd], B, A);
          });
        });
      }
      unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
        simd<int, 16> dm = d[m / 8].template select<16, 1>((m % 8) * 16);
        simd<int64_t, 16> wide = dm;
        acc[m] += wide * (int64_t)Aw[m * G + g];
      });
    }
    unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value; block_store<int64_t, 16>(part + ((size_t)wk * MR + m) * N + r0, acc[m]); });
  });
}
}  // namespace yx
