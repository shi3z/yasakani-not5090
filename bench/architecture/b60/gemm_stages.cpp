// SPDX-License-Identifier: Apache-2.0
// Stage decomposition of the compressed GEMM (same kernel structure and data layouts as the engine's GEMM, synthetic data).
//   acc[m][n] = sum over 512-column groups g of Aw[m][g] * sum_{c in g} w8(n, c) * qx[m][c]      (exact int64)
// Stages (each includes the previous ones):
//   A read the packed codes        B + extract the 12-bit states      C + value-table lookup (SLM gather)
//   D + build the int8 operand     E + dpas (int32 sums)              F + int64 accumulation    G + scale to float
// Structures: "small" = one thread does 16 output rows x all MR input rows (MR = 1, 8, 16; sums in registers);
//             "big"   = one thread does 16 output rows x 8*TR input rows, input rows in SLM, int64 sums in memory (M = 32, 64).
// Layouts: stream [Kc/256][N/32][BITS][32 lanes][4 dwords], lut uint32[4096] (byte 0 / byte 1 = the two weights of a state),
//          qx [Kc/32][MR][32] int8, Aw [MR][G] int32, part [MR][N] int64.
// usage: gemm_stages [shapes]     shapes = comma list of NxKc, default 34816x5120,5120x17408,20480x17408
//        env ARCH_ONLY=small,planar,big   ARCH_BITS=6,8   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
constexpr int LS = 32;
struct Bufs { uint32_t* stream; uint32_t* lut; int8_t* qx; int32_t* Aw; float* us; float* tau; void* out; float* outf; };

template <int BITS, int J> inline __attribute__((always_inline)) simd<uint32_t, 16> state_of(const simd<uint32_t, 16>* w) {
  constexpr int a = BITS * (J + 1) - 12;
  if constexpr (a < 0) return (w[0] << (uint32_t)(-a)) & 0xfffu;
  else { constexpr int wi = a >> 5, sh = a & 31;
    if constexpr (sh + 12 <= 32) return (w[wi] >> (uint32_t)sh) & 0xfffu;
    else return ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0xfffu; }
}

// The same state already multiplied by 4 (the SLM byte offset): one shift (pair) and one mask, no separate scaling.
template <int BITS, int J> inline __attribute__((always_inline)) simd<uint32_t, 16> offset_of(const simd<uint32_t, 16>* w) {
  constexpr int a = BITS * (J + 1) - 14;
  if constexpr (a < 0) return (w[0] << (uint32_t)(-a)) & 0x3ffcu;
  else { constexpr int wi = a >> 5, sh = a & 31;
    if constexpr (sh + 14 <= 32) return (w[wi] >> (uint32_t)sh) & 0x3ffcu;
    else return ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0x3ffcu; }
}

// PL = 1: "planar" code layout// PL = 2: planar + states extracted directly as SLM offsets (offset_of)
// PL = 3: as 2 + a second copy of the table shifted left by 16 (32 KiB of SLM): the two halves of a dword are just OR-ed
// PL = 1: "planar" code layout [..][BITS][4 dwords][16 lanes] per 16 rows instead of the file layout [..][BITS][lane][4 dwords]
template <int BITS, int MR, int ST, int PL = 0>
event k_small(queue& q, Bufs b, int N, int Kc) {
  constexpr int NW = 4 * BITS, D = MR > 8 ? 8 : MR, ND = MR > 8 ? 2 : 1;
  const int G = Kc / 512, rows32 = N / 32;
  const uint32_t* stream = b.stream; const uint32_t* lut = b.lut; const int8_t* qx = b.qx; const int32_t* Aw = b.Aw;
  const float* us = b.us; const float* tau = b.tau; void* out = b.out;
  return q.parallel_for(nd_range<1>(N / 16, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<PL == 3 ? 32768 : 16384>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += LS) { const simd<uint32_t, 64> t = block_load<uint32_t, 64>(lut + i * 64);
      slm_block_store<uint32_t, 64>(i * 256, t);
      if constexpr (PL == 3) slm_block_store<uint32_t, 64>(16384 + i * 256, t << 16u); }      // second copy, already in the high half
    barrier();
    const uint32_t r0 = 16u * (uint32_t)it.get_global_id(0), lane0 = r0 & 31u, gr = r0 >> 5;
    simd<uint32_t, 16> chk = 0; simd<uint32_t, 64> chk64 = 0;
    simd<int64_t, 16> acc[MR]; simd<int, 16 * D> s32[ND];
    unroll<MR>([&](auto mi) { acc[decltype(mi)::value] = 0; });
    unroll<ND>([&](auto di) { s32[decltype(di)::value] = 0; });
    for (int g = 0; g < G; ++g) {
      simd<int, 16 * D> d[ND];
      unroll<ND>([&](auto di) { d[decltype(di)::value] = 0; });
      for (int h = 0; h < 2; ++h) {
        const int s = 2 * g + h;
        const uint32_t gq = (uint32_t)s * rows32 + gr;
        simd<uint32_t, 16> w[NW + 1];
        unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
          simd<uint32_t, 64> blk = block_load<uint32_t, 64>(stream + ((size_t)(gq * BITS + c) * 32 + lane0) * 4);
          if constexpr (ST == 0) chk64 ^= blk;
          else if constexpr (PL >= 1) unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = blk.template select<16, 1>(16 * k); });
          else unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = blk.template select<16, 4>(k); });
        });
        if constexpr (ST >= 1) unroll<8>([&](auto chi) { constexpr int ch = decltype(chi)::value;
          simd<int, 128> B;
          unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
            simd<uint32_t, 16> st0, st1;
            if constexpr (PL >= 2) { st0 = offset_of<BITS, 16 * ch + 2 * i>(w); st1 = offset_of<BITS, 16 * ch + 2 * i + 1>(w); }
            else { st0 = state_of<BITS, 16 * ch + 2 * i>(w); st1 = state_of<BITS, 16 * ch + 2 * i + 1>(w); }
            if constexpr (ST == 1) chk += st0 + st1;
            else {
              if constexpr (PL < 2) { st0 = st0 * 4u; st1 = st1 * 4u; }
              if constexpr (PL == 3) st1 += 16384u;
              const simd<uint32_t, 16> g0 = slm_gather<uint32_t, 16>(st0), g1 = slm_gather<uint32_t, 16>(st1);
              if constexpr (ST == 2) chk += g0 + g1;
              else if constexpr (ST == 3) chk ^= (g0 & 0xffffu) | (g1 << 16u);
              else if constexpr (PL == 3) B.template select<16, 1>(i * 16) = simd<int, 16>(g0 | g1);     // table entries are 16 bit
              else B.template select<16, 1>(i * 16) = simd<int, 16>((g0 & 0xffffu) | (g1 << 16u));
            }
          });
          if constexpr (ST >= 4) {
            const int8_t* xa = qx + (size_t)(s * 8 + ch) * MR * 32;
            unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
              simd<int, 8 * D> A = block_load<int, 8 * D>((const int*)(xa + dd * 8 * 32));
              d[dd] = xmx::dpas<8, D, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(d[dd], B, A);
            });
          }
        });
      }
      if constexpr (ST == 4) unroll<ND>([&](auto di) { s32[decltype(di)::value] += d[decltype(di)::value]; });
      if constexpr (ST >= 5) unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
        simd<int, 16> dm = d[m / 8].template select<16, 1>((m % 8) * 16);
        simd<int64_t, 16> wide = dm;
        acc[m] += wide * (int64_t)Aw[m * G + g];
      });
    }
    if constexpr (ST == 0) { unroll<4>([&](auto ki) { chk ^= chk64.template select<16, 1>(16 * decltype(ki)::value); }); }
    if constexpr (ST < 4) block_store<uint32_t, 16>((uint32_t*)out + r0, chk);
    else if constexpr (ST == 4) unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
      block_store<int, 16>((int*)out + (size_t)m * N + r0, s32[m / 8].template select<16, 1>((m % 8) * 16)); });
    else if constexpr (ST == 5) unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
      block_store<int64_t, 16>((int64_t*)out + (size_t)m * N + r0, acc[m]); });
    else { const simd<float, 16> tv = block_load<float, 16>(tau + r0);
      unroll<MR>([&](auto mi) { constexpr int m = decltype(mi)::value;
        simd<float, 16> f = acc[m]; f = f * us[m]; f = f * tv;
        block_store<float, 16>((float*)out + (size_t)m * N + r0, f); }); }
  });
}

// ST 4: int32 sums accumulated in memory, 5: int64 (as the engine), 6: + float result at the last group.  MR = RS * 8 * TR rows.
template <int BITS, int TR, int ST>
event k_big(queue& q, Bufs b, int N, int Kc, int MR) {
  constexpr int NW = 4 * BITS;
  const int G = Kc / 512, RS = MR / (8 * TR), rows32 = N / 32;
  const uint32_t* stream = b.stream; const uint32_t* lut = b.lut; const int8_t* qx = b.qx; const int32_t* Aw = b.Aw;
  const float* us = b.us; const float* tau = b.tau; void* out = b.out; float* outf = b.outf;
  return q.parallel_for(nd_range<1>((size_t)(N / 16) * RS, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<16384 + TR * 4096>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t gid = (uint32_t)it.get_global_id(0), nrt = (uint32_t)N / 16u, rs = gid / nrt, rt = gid - rs * nrt;
    const int m0 = (int)rs * 8 * TR;
    const uint32_t r0 = 16u * rt, lane0 = r0 & 31u, gr = r0 >> 5;
    for (int g = 0; g < G; ++g) {
      { const int c16 = (int)lid >> 1, hf = (int)lid & 1; const int* src = (const int*)(qx + ((size_t)(g * 16 + c16) * MR + m0 + hf * 4 * TR) * 32);
        unroll<TR>([&](auto ti) { constexpr int tt = decltype(ti)::value;
          slm_block_store<int, 32>(16384 + (c16 * 8 * TR + hf * 4 * TR) * 32 + tt * 128, block_load<int, 32>(src + tt * 32)); }); }
      barrier();
      simd<int, 16> d[TR * 8];
      unroll<TR * 8>([&](auto di) { d[decltype(di)::value] = 0; });
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
            const simd<uint32_t, 16> g0 = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + 2 * i>(w) * 4u);
            const simd<uint32_t, 16> g1 = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + 2 * i + 1>(w) * 4u);
            B.template select<16, 1>(i * 16) = simd<int, 16>((g0 & 0xffffu) | (g1 << 16u));
          });
          unroll<TR>([&](auto di) { constexpr int dd = decltype(di)::value;
            simd<int, 64> A = slm_block_load<int, 64>(16384 + ((h * 8 + ch) * 8 * TR + dd * 8) * 32);
            unroll<8>([&](auto mi) { constexpr int m = decltype(mi)::value; const simd<int, 8> a1 = A.template select<8, 1>(8 * m);
              d[dd * 8 + m] = xmx::dpas<8, 1, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(d[dd * 8 + m], B, a1); });
          });
        });
      }
      unroll<TR * 8>([&](auto di) { constexpr int j = decltype(di)::value;
        const int row = m0 + j;
        if constexpr (ST == 4) { int* pp = (int*)out + (size_t)row * N + r0; simd<int, 16> v = d[j];
          if (g) v += block_load<int, 16>(pp);
          block_store<int, 16>(pp, v); }
        else { int64_t* pp = (int64_t*)out + (size_t)row * N + r0;
          simd<int64_t, 16> wide = d[j]; wide = wide * (int64_t)Aw[row * G + g];
          if (g) wide += block_load<int64_t, 16>(pp);
          block_store<int64_t, 16>(pp, wide);
          if constexpr (ST == 6) if (g == G - 1) { simd<float, 16> f = wide; f = f * us[row]; f = f * block_load<float, 16>(tau + r0);
            block_store<float, 16>(outf + (size_t)row * N + r0, f); } }
      });
      barrier();
    }
  });
}

// Host reference of the exact int64 result for the 16 output rows starting at r0 (all MR rows).
static std::vector<int64_t> host_ref(int BITS, const std::vector<uint32_t>& stream, const std::vector<uint32_t>& lut, const std::vector<int8_t>& qx,
                                     const std::vector<int32_t>& Aw, int N, int Kc, int MR, int r0, bool planar = false) {
  const int G = Kc / 512, rows32 = N / 32, lane0 = r0 & 31, gr = r0 >> 5; std::vector<int64_t> ref((size_t)MR * 16, 0);
  for (int l = 0; l < 16; ++l) for (int g = 0; g < G; ++g) { std::vector<int64_t> dot(MR, 0);
    for (int h = 0; h < 2; ++h) { const int s = 2 * g + h; const size_t gq = (size_t)s * rows32 + gr; uint32_t w[33] = {0};
      for (int c = 0; c < BITS; ++c) for (int k = 0; k < 4; ++k) w[c * 4 + k] = planar ? stream[((gq * BITS + c) * 32 + lane0) * 4 + k * 16 + l] : stream[((gq * BITS + c) * 32 + lane0 + l) * 4 + k];
      for (int j = 0; j < 128; ++j) { const int a = BITS * (j + 1) - 12; uint32_t st;
        if (a < 0) st = (w[0] << (-a)) & 0xfffu; else { const int wi = a >> 5, sh = a & 31; uint64_t v = w[wi] | ((uint64_t)w[wi + 1] << 32); st = (uint32_t)(v >> sh) & 0xfffu; }
        for (int e = 0; e < 2; ++e) { const int wv = (int8_t)(lut[st] >> (8 * e)), col = 2 * j + e;
          for (int m = 0; m < MR; ++m) dot[m] += wv * (int)qx[((size_t)(s * 8 + col / 32) * MR + m) * 32 + col % 32]; } } }
    for (int m = 0; m < MR; ++m) ref[m * 16 + l] += (int64_t)(int32_t)dot[m] * Aw[m * G + g]; }
  return ref;
}

int main(int argc, char** argv) {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "gemm_stages");
  std::vector<std::pair<int, int>> shapes;
  { std::string s = argc > 1 ? argv[1] : "34816x5120,5120x17408,20480x17408"; size_t p = 0;
    while (p < s.size()) { size_t e = s.find(',', p); if (e == std::string::npos) e = s.size(); int n, k;
      if (std::sscanf(s.substr(p, e - p).c_str(), "%dx%d", &n, &k) == 2) shapes.push_back({n, k}); p = e + 1; } }
  const char* be = getenv("ARCH_BITS"); const std::string bsel = be ? be : "6,8";
  std::printf("shape,bits_per_weight,structure,rows_M,rows_per_thread,stage,threads,ms_queued,ms_sync,Gweights_per_s,code_GB_per_s,check\n");
  const char* stn[] = {"A_read", "B_states", "C_lookup", "D_operand", "E_dpas", "F_int64", "G_float"};
  for (auto [N, Kc] : shapes) for (int BITS : {6, 8}) {
    if (bsel.find(BITS == 6 ? '6' : '8') == std::string::npos) continue;
    const int G = Kc / 512, MAXM = 256; uint64_t seed = 12345 + BITS;
    const size_t nstream = (size_t)N * Kc * BITS / 64;      // dwords
    std::vector<uint32_t> hs(nstream), hl(4096); std::vector<int8_t> hq((size_t)Kc * MAXM); std::vector<int32_t> ha((size_t)MAXM * G);
    for (auto& v : hs) v = ab::rng(seed);
    for (auto& v : hl) v = ab::rng(seed) & 0xffffu;
    for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    for (auto& v : ha) v = (int32_t)(ab::rng(seed) >> 8);
    std::vector<float> hu(MAXM, 1.0f / 16777216.f), ht(N, 0.37f);
    Bufs b; b.stream = malloc_device<uint32_t>(nstream, q); b.lut = malloc_device<uint32_t>(4096, q); b.qx = malloc_device<int8_t>(hq.size(), q);
    b.Aw = malloc_device<int32_t>(ha.size(), q); b.us = malloc_device<float>(MAXM, q); b.tau = malloc_device<float>(N, q);
    b.out = malloc_device<int64_t>((size_t)MAXM * N, q); b.outf = malloc_device<float>((size_t)MAXM * N, q);
    q.memcpy(b.stream, hs.data(), nstream * 4); q.memcpy(b.lut, hl.data(), 16384); q.memcpy(b.qx, hq.data(), hq.size());
    q.memcpy(b.Aw, ha.data(), ha.size() * 4); q.memcpy(b.us, hu.data(), MAXM * 4); q.memcpy(b.tau, ht.data(), (size_t)N * 4); q.wait();
    const double gw = (double)N * Kc / 1e9, gb = (double)nstream * 4 / 1e9;
    auto report = [&](const char* structure, int M, int rpt, int st, int threads, auto&& launch, const char* chk) {
      const double tq = ab::best_time(q, launch, false), ts = ab::best_time(q, launch, true);
      std::printf("%dx%d,%d,%s,%d,%d,%s,%d,%.4f,%.4f,%.1f,%.1f,%s\n", N, Kc, BITS / 2, structure, M, rpt, stn[st], threads, tq * 1e3, ts * 1e3, gw / tq, gb / tq, chk);
      std::fflush(stdout); };
    // exact check of stage F against the host for two threads
    auto check = [&](int MR, bool planar) -> const char* {
      // qx / Aw layouts depend on MR: the device buffers are simply reinterpreted, so the host does the same
      std::vector<int64_t> dv((size_t)MR * N); q.memcpy(dv.data(), b.out, dv.size() * 8).wait();
      for (int r0 : {0, (N / 2) & ~15}) { auto ref = host_ref(BITS, hs, hl, hq, ha, N, Kc, MR, r0, planar);
        for (int m = 0; m < MR; ++m) for (int l = 0; l < 16; ++l) if (dv[(size_t)m * N + r0 + l] != ref[m * 16 + l]) return "MISMATCH"; }
      return "exact"; };
    auto small = [&]<int B, int MR, int ST>() {
      auto launch = [&] { return k_small<B, MR, ST>(q, b, N, Kc); };
      const char* c = "-"; if (ST == 5) { launch().wait(); c = check(MR, false); }
      report("small", MR, MR, ST, N / 16, launch, c); };
    auto planar = [&]<int B, int MR, int ST, int PL = 1>() {
      auto launch = [&] { return k_small<B, MR, ST, PL>(q, b, N, Kc); };
      const char* c = "-"; if (ST == 5) { launch().wait(); c = check(MR, true); }
      report(PL == 3 ? "small_planar_direct_2tables" : PL == 2 ? "small_planar_codes_direct_offsets" : "small_planar_codes", MR, MR, ST, N / 16, launch, c); };
    auto big = [&]<int B, int TR, int ST>(int M) {
      auto launch = [&] { return k_big<B, TR, ST>(q, b, N, Kc, M); };
      const char* c = "-"; if (ST == 5) { launch().wait(); c = check(M, false); }
      report("big", M, 8 * TR, ST, N / 16 * (M / (8 * TR)), launch, c); };
    auto all = [&]<int B>() {
      if (ab::want("small")) {
        small.template operator()<B, 1, 0>(); small.template operator()<B, 1, 1>(); small.template operator()<B, 1, 2>(); small.template operator()<B, 1, 3>();
        small.template operator()<B, 1, 4>(); small.template operator()<B, 1, 5>(); small.template operator()<B, 1, 6>();
        small.template operator()<B, 8, 4>(); small.template operator()<B, 8, 5>(); small.template operator()<B, 8, 6>();
        small.template operator()<B, 16, 4>(); small.template operator()<B, 16, 5>(); small.template operator()<B, 16, 6>();
      }
      if (ab::want("planar")) {
        planar.template operator()<B, 1, 1>(); planar.template operator()<B, 1, 2>(); planar.template operator()<B, 1, 3>();
        planar.template operator()<B, 1, 5>(); planar.template operator()<B, 8, 5>(); planar.template operator()<B, 16, 5>();
        planar.template operator()<B, 1, 1, 2>(); planar.template operator()<B, 1, 3, 2>();
        planar.template operator()<B, 1, 5, 2>(); planar.template operator()<B, 8, 5, 2>(); planar.template operator()<B, 16, 5, 2>();
        planar.template operator()<B, 1, 5, 3>(); planar.template operator()<B, 8, 5, 3>(); planar.template operator()<B, 16, 5, 3>();
      }
      if (ab::want("big")) for (int M : {32, 64}) {
        big.template operator()<B, 4, 4>(M); big.template operator()<B, 4, 5>(M); big.template operator()<B, 4, 6>(M);
        if (M == 64) { big.template operator()<B, 2, 5>(M); big.template operator()<B, 8, 5>(M); }
      }
      if (ab::want("big")) for (int M : {128, 256}) big.template operator()<B, 4, 5>(M);      // prefill batches: M / 32 decodes of every weight
    };
    if (BITS == 6) all.template operator()<6>(); else all.template operator()<8>();
    free(b.stream, q); free(b.lut, q); free(b.qx, q); free(b.Aw, q); free(b.us, q); free(b.tau, q); free(b.out, q); free(b.outf, q);
  }
  return 0;
}
