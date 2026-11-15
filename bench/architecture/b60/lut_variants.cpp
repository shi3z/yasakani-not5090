// SPDX-License-Identifier: Apache-2.0
// Value-table variants for the compressed GEMM on the B60.
//  micro : lookups per second of one table-lookup primitive in a tight loop (index = scalar or vector LCG)
//          none | slm (16 KiB table in SLM) | usm (16 KiB table in global memory) | usm1m (1 MiB table) | iselect<TN> (table in a simd object)
//          index patterns: random (16 different entries), blocked (16 consecutive entries = one 64-byte line), const (one entry)
//  kernel: the whole GEMM kernel (3-bit codes, 8 input rows, int32 sums) with the lookup replaced:
//          none (state bits used as weights) | slm | usm | usm18 (1 MiB table indexed by two consecutive states: 4 weights per gather)
//          and the slm variant with a constant code stream (all lookups hit one entry).
// usage: lut_variants      env ARCH_ONLY=micro,kernel   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
constexpr uint32_t LA = 1664525u, LC = 1013904223u;
constexpr int LS = 32;

// KIND 0 none, 1 slm, 2 usm (mask given), PAT 0 random, 1 blocked, 2 const
template <int KIND, int PAT>
event k_micro(queue& q, const uint32_t* tab, uint32_t* out, int T, int iters, uint32_t mask) {
  return q.parallel_for(nd_range<1>(T, LS), [=](nd_item<1> it_) SYCL_ESIMD_KERNEL {
    const uint32_t lid = it_.get_local_id(0), gid = (uint32_t)it_.get_global_id(0);
    if constexpr (KIND == 1) { slm_init<16384>(); for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(tab + i * 64)); barrier(); }
    simd<uint32_t, 16> idx(0x9E3779B1u * (gid + 1u), 0x85EBCA6Bu), acc = 0; uint32_t sc = gid * 0x9E3779B1u + 1u; const simd<uint32_t, 16> lanes(0u, 1u);
    for (int it = 0; it < iters; ++it) {
      simd<uint32_t, 16> ix;
      if constexpr (PAT == 0) { idx = idx * LA + LC; ix = idx & mask; }
      else if constexpr (PAT == 1) { sc = sc * LA + LC; ix = lanes + ((sc >> 8) & mask & ~15u); }
      else { sc = sc * LA + LC; ix = simd<uint32_t, 16>((sc >> 8) & mask); }
      if constexpr (KIND == 0) acc += ix << 2u;
      else if constexpr (KIND == 1) acc += slm_gather<uint32_t, 16>(ix << 2u);
      else acc += gather<uint32_t, 16>(tab, ix << 2u);
    }
    block_store<uint32_t, 16>(out + gid * 16, acc); });
}
// Lanes per gather message: GN = 8, 16 or 32 random SLM (KIND 1) or global (KIND 2) lookups in ONE message.
template <int KIND, int GN>
event k_width(queue& q, const uint32_t* tab, uint32_t* out, int T, int iters, uint32_t mask) {
  return q.parallel_for(nd_range<1>(T, LS), [=](nd_item<1> it_) SYCL_ESIMD_KERNEL {
    const uint32_t lid = it_.get_local_id(0), gid = (uint32_t)it_.get_global_id(0);
    if constexpr (KIND == 1) { slm_init<16384>(); for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(tab + i * 64)); barrier(); }
    simd<uint32_t, GN> idx(0x9E3779B1u * (gid + 1u), 0x85EBCA6Bu), acc = 0;
    for (int it = 0; it < iters; ++it) { idx = idx * LA + LC;
      if constexpr (KIND == 1) acc += slm_gather<uint32_t, GN>((idx & mask) << 2u);
      else if constexpr (KIND == 2) acc += gather<uint32_t, GN>(tab, (idx & mask) << 2u);
      else acc += (idx & mask) << 2u; }
    simd<uint32_t, 16> s = 0;
    unroll<GN / 8>([&](auto c) { s.template select<8, 1>(0) += acc.template select<8, 1>(8 * decltype(c)::value); });
    block_store<uint32_t, 16>(out + gid * 16, s); });
}
template <int TN>
event k_isel(queue& q, const uint16_t* tab, uint32_t* out, int T, int iters) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint16_t, TN> tb; tb.copy_from(tab);
    simd<uint32_t, 16> idx(0x9E3779B1u * ((uint32_t)i[0] + 1u), 0x85EBCA6Bu), acc = 0;
    for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; const simd<uint16_t, 16> ix16 = idx & (uint32_t)(TN - 1);
      const simd<uint16_t, 16> v = tb.iselect(ix16); acc += simd<uint32_t, 16>(v); }
    block_store<uint32_t, 16>(out + i[0] * 16, acc); });
}

template <int BITS, int J> inline __attribute__((always_inline)) simd<uint32_t, 16> bits_of(const simd<uint32_t, 16>* w, uint32_t msk) {   // state J, optionally wider
  constexpr int a = BITS * (J + 1) - 12;
  if constexpr (a < 0) return (w[0] << (uint32_t)(-a)) & msk;
  else { constexpr int wi = a >> 5, sh = a & 31;
    if constexpr (sh == 0) return w[wi] & msk;
    else return ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & msk; }
}
// LK 0 none, 1 slm, 2 usm, 3 usm18, 4 slm with ONE 32-lane gather per dword of the operand (both states in one message)
template <int LK>
event k_gemm(queue& q, const uint32_t* stream, const uint32_t* lut, const uint32_t* lut18, const int8_t* qx, int* out, int N, int Kc) {
  constexpr int BITS = 6, MR = 8, NW = 4 * BITS;
  const int S = Kc / 256, rows32 = N / 32;
  return q.parallel_for(nd_range<1>(N / 16, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<16384>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t r0 = 16u * (uint32_t)it.get_global_id(0), lane0 = r0 & 31u, gr = r0 >> 5;
    simd<int, 16 * MR> acc = 0;
    for (int s = 0; s < S; ++s) {
      const uint32_t gq = (uint32_t)s * rows32 + gr;
      simd<uint32_t, 16> w[NW + 1]; w[NW] = 0;
      unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
        simd<uint32_t, 64> blk = block_load<uint32_t, 64>(stream + ((size_t)(gq * BITS + c) * 32 + lane0) * 4);
        unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = blk.template select<16, 4>(k); }); });
      unroll<8>([&](auto chi) { constexpr int ch = decltype(chi)::value;
        simd<int, 128> B;
        unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value; constexpr int j = 16 * ch + 2 * i;
          simd<uint32_t, 16> v;
          if constexpr (LK == 3) v = gather<uint32_t, 16>(lut18, bits_of<BITS, j>(w, 0x3ffffu) << 2u);
          else { const simd<uint32_t, 16> s0 = bits_of<BITS, j>(w, 0xfffu), s1 = bits_of<BITS, j + 1>(w, 0xfffu);
            if constexpr (LK == 0) v = s0 | (s1 << 16u);
            else if constexpr (LK == 1) v = (slm_gather<uint32_t, 16>(s0 << 2u) & 0xffffu) | (slm_gather<uint32_t, 16>(s1 << 2u) << 16u);
            else if constexpr (LK == 4) { simd<uint32_t, 32> off; off.template select<16, 1>(0) = s0 << 2u; off.template select<16, 1>(16) = s1 << 2u;
              simd<uint32_t, 32> g = slm_gather<uint32_t, 32>(off);
              v = (g.template select<16, 1>(0) & 0xffffu) | (g.template select<16, 1>(16) << 16u); }
            else v = (gather<uint32_t, 16>(lut, s0 << 2u) & 0xffffu) | (gather<uint32_t, 16>(lut, s1 << 2u) << 16u); }
          B.template select<16, 1>(i * 16) = simd<int, 16>(v); });
        simd<int, 8 * MR> A = block_load<int, 8 * MR>((const int*)(qx + (size_t)(s * 8 + ch) * MR * 32));
        acc = xmx::dpas<8, MR, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(acc, B, A); });
    }
    unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value; block_store<int, 16>(out + (size_t)m * N + r0, acc.template select<16, 1>(m * 16)); });
  });
}

int main() {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "lut_variants");
  uint64_t seed = 7;
  std::vector<uint32_t> hl(1 << 18); for (auto& v : hl) v = ab::rng(seed) & 0xffffu;
  std::vector<uint32_t> h18(1 << 18); for (uint32_t i = 0; i < (1u << 18); ++i) h18[i] = (hl[i & 0xfff] & 0xffffu) | (hl[i >> 6] << 16);
  std::vector<uint16_t> h16(4096); for (int i = 0; i < 4096; ++i) h16[i] = (uint16_t)hl[i];
  uint32_t* tab = malloc_device<uint32_t>(1 << 18, q); uint32_t* t18 = malloc_device<uint32_t>(1 << 18, q); uint16_t* t16 = malloc_device<uint16_t>(4096, q);
  uint32_t* out = malloc_device<uint32_t>(16 * 65536, q);
  q.memcpy(tab, hl.data(), 4 << 18); q.memcpy(t18, h18.data(), 4 << 18); q.memcpy(t16, h16.data(), 8192); q.wait();
  if (ab::want("micro")) {
    std::printf("section,primitive,pattern,table_bytes,threads,iters,ms,G_lookups_per_s,ns_per_gather16_in_thread\n");
    const char* pn[] = {"random", "blocked", "const"};
    auto row = [&](const char* prim, const char* pat, int bytes, int T, int iters, double s) {
      std::printf("micro,%s,%s,%d,%d,%d,%.4f,%.2f,%.2f\n", prim, pat, bytes, T, iters, s * 1e3, (double)T * iters * 16 / s / 1e9, s * 1e9 / iters); std::fflush(stdout); };
    for (int T : {640, 4096}) { const int it = 4096;
      auto m = [&]<int KIND, int PAT>(const char* prim, uint32_t mask) { row(prim, pn[PAT], (int)(mask + 1) * 4, T, it, ab::best_time(q, [&] { return k_micro<KIND, PAT>(q, tab, out, T, it, mask); }, true)); };
      m.template operator()<0, 0>("none", 4095); m.template operator()<0, 1>("none", 4095); m.template operator()<0, 2>("none", 4095);
      m.template operator()<1, 0>("slm_gather", 4095); m.template operator()<1, 1>("slm_gather", 4095); m.template operator()<1, 2>("slm_gather", 4095);
      m.template operator()<2, 0>("usm_gather", 4095); m.template operator()<2, 1>("usm_gather", 4095); m.template operator()<2, 2>("usm_gather", 4095);
      m.template operator()<2, 0>("usm_gather", (1u << 18) - 1); m.template operator()<2, 1>("usm_gather", (1u << 18) - 1);
      auto wd = [&]<int KIND, int GN>(const char* prim) { const double sec = ab::best_time(q, [&] { return k_width<KIND, GN>(q, tab, out, T, it, 4095u); }, true);
        std::printf("micro,%s,random,%d,%d,%d,%.4f,%.2f,%.2f\n", prim, 16384, T, it, sec * 1e3, (double)T * it * GN / sec / 1e9, sec * 1e9 / it); std::fflush(stdout); };
      wd.template operator()<0, 8>("none 8 lanes/iteration"); wd.template operator()<0, 32>("none 32 lanes/iteration");
      wd.template operator()<1, 8>("slm_gather 8 lanes/message"); wd.template operator()<1, 16>("slm_gather 16 lanes/message"); wd.template operator()<1, 32>("slm_gather 32 lanes/message");
      wd.template operator()<2, 8>("usm_gather 8 lanes/message"); wd.template operator()<2, 16>("usm_gather 16 lanes/message"); wd.template operator()<2, 32>("usm_gather 32 lanes/message");
      auto is = [&]<int TN>() { row("iselect(simd<u16,TN>)", "random", TN * 2, T, it, ab::best_time(q, [&] { return k_isel<TN>(q, t16, out, T, it); }, true)); };
      is.template operator()<32>(); is.template operator()<256>(); is.template operator()<1024>(); is.template operator()<2048>();   // TN = 4096 (8 KiB = the whole 128 x 64-byte register file) does not compile: "VISA builder API call failed"
    }
  }
  if (ab::want("kernel")) {
    const int N = 34816, Kc = 5120, BITS = 6; const size_t nstream = (size_t)N * Kc * BITS / 64;
    std::vector<uint32_t> hs(nstream); for (auto& v : hs) v = ab::rng(seed);
    std::vector<int8_t> hq((size_t)Kc * 8); for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    uint32_t* st = malloc_device<uint32_t>(nstream, q); uint32_t* st0 = malloc_device<uint32_t>(nstream, q); int8_t* qx = malloc_device<int8_t>(hq.size(), q);
    uint32_t* st1 = malloc_device<uint32_t>(nstream, q); uint32_t* st2 = malloc_device<uint32_t>(nstream, q);
    q.memset(st1, 0xff, nstream * 4);                                        // all ones: every state is 4095
    { std::vector<uint32_t> h2(nstream);                                     // the 32 rows of a block share one code: the 16 lanes of a gather hit one entry
      for (size_t i = 0; i < nstream; i += 128) { uint32_t w4[4]; for (auto& v : w4) v = ab::rng(seed); for (size_t j = 0; j < 128 && i + j < nstream; ++j) h2[i + j] = w4[j & 3]; }
      q.memcpy(st2, h2.data(), nstream * 4).wait(); }
    int* o = malloc_device<int>((size_t)8 * N, q); int* o2 = malloc_device<int>((size_t)8 * N, q);
    q.memcpy(st, hs.data(), nstream * 4); q.memset(st0, 0, nstream * 4); q.memcpy(qx, hq.data(), hq.size()); q.wait();
    std::printf("section,lookup,code_stream,ms_queued,ms_sync,Gweights_per_s,check\n");
    auto row = [&]<int LK>(const char* name, const uint32_t* s, const char* sn, const char* chk) {
      auto l = [&] { return k_gemm<LK>(q, s, tab, t18, qx, o, N, Kc); };
      const double tq = ab::best_time(q, l, false), ts = ab::best_time(q, l, true);
      std::printf("kernel,%s,%s,%.4f,%.4f,%.1f,%s\n", name, sn, tq * 1e3, ts * 1e3, (double)N * Kc / tq / 1e9, chk); std::fflush(stdout); };
    // the three table variants must give identical sums
    k_gemm<1>(q, st, tab, t18, qx, o, N, Kc).wait(); k_gemm<2>(q, st, tab, t18, qx, o2, N, Kc).wait();
    std::vector<int> a((size_t)8 * N), b((size_t)8 * N), c((size_t)8 * N); q.memcpy(a.data(), o, a.size() * 4).wait(); q.memcpy(b.data(), o2, b.size() * 4).wait();
    k_gemm<3>(q, st, tab, t18, qx, o2, N, Kc).wait(); q.memcpy(c.data(), o2, c.size() * 4).wait();
    const char* c2 = a == b ? "same sums as slm" : "DIFFERENT", *c3 = a == c ? "same sums as slm" : "DIFFERENT";
    row.template operator()<0>("none (state bits as weights)", st, "random", "-");
    row.template operator()<1>("slm gather (engine)", st, "random", "reference");
    row.template operator()<1>("slm gather (engine)", st0, "all zero (one entry)", "-");
    row.template operator()<0>("none (state bits as weights)", st0, "all zero (one entry)", "-");
    row.template operator()<0>("none (state bits as weights)", st1, "all ones (one entry)", "-");
    row.template operator()<0>("none (state bits as weights)", st2, "same code in all rows of a block", "-");
    row.template operator()<1>("slm gather (engine)", st1, "all ones (one entry)", "-");
    row.template operator()<1>("slm gather (engine)", st2, "same code in all rows of a block", "-");
    k_gemm<4>(q, st, tab, t18, qx, o2, N, Kc).wait(); q.memcpy(c.data(), o2, c.size() * 4).wait();
    row.template operator()<4>("slm gather, 32 lanes per message", st, "random", a == c ? "same sums as slm" : "DIFFERENT");
    row.template operator()<4>("slm gather, 32 lanes per message", st2, "same code in all rows of a block", "-");
    row.template operator()<2>("usm gather 16 KiB", st, "random", c2);
    row.template operator()<2>("usm gather 16 KiB", st0, "all zero (one entry)", "-");
    row.template operator()<3>("usm gather 1 MiB pair table (4 weights/gather)", st, "random", c3);
    row.template operator()<3>("usm gather 1 MiB pair table (4 weights/gather)", st0, "all zero (one entry)", "-");
  }
  return 0;
}
