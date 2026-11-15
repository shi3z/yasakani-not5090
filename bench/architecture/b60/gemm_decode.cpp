// SPDX-License-Identifier: Apache-2.0
// Decode-time compressed GEMM (1 to 16 input rows): variants of the engine kernel, same math and layouts as gemm_stages.cpp.
//   PF = 0  engine kernel: the six code blocks of a half group are loaded at the top of the trip that decodes them
//   PF = 1  one trip ahead: the blocks of trip s + 1 are requested before the lookups of trip s start
//   PF = 2  as 1, but requested in the middle of trip s (fewer live registers)
//   GA = 1  the 16 table lookups of a 32-column chunk are requested before the first result is used;  GA = 2: addresses first, then lookups
//   GA = 3  every second lookup from a copy of the table in global memory;  GA = 4: one lookup in four
// W = column workers per matrix (threads = N / 16 * W, each over G / W column groups; partial sums are added by the caller).
// usage: gemm_decode [shapes]      shapes = comma list of NxKc, default 34816x5120,5120x17408,16384x5120,5120x6144
//        env ARCH_BITS=6,8
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
constexpr int LS = 32;
struct Bufs { uint32_t* stream; uint32_t* lut; int8_t* qx; int32_t* Aw; int64_t* out; };

template <int BITS, int J> inline __attribute__((always_inline)) simd<uint32_t, 16> state_of(const simd<uint32_t, 16>* w) {
  constexpr int a = BITS * (J + 1) - 12;
  if constexpr (a < 0) return (w[0] << (uint32_t)(-a)) & 0xfffu;
  else { constexpr int wi = a >> 5, sh = a & 31;
    if constexpr (sh + 12 <= 32) return (w[wi] >> (uint32_t)sh) & 0xfffu;
    else return ((w[wi] >> (uint32_t)sh) | (w[wi + 1] << (uint32_t)(32 - sh))) & 0xfffu; }
}

template <int BITS, int MR, int PF, int GA = 0>
event k_dec(queue& q, Bufs b, int N, int Kc, int W) {
  constexpr int NW = 4 * BITS, D = MR > 8 ? 8 : MR, ND = MR > 8 ? 2 : 1;
  const int G = Kc / 512, rows32 = N / 32;
  const uint32_t* stream = b.stream; const uint32_t* lut = b.lut; const int8_t* qx = b.qx; const int32_t* Aw = b.Aw; int64_t* part = b.out;
  return q.parallel_for(nd_range<1>((size_t)(N / 16) * W, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<16384>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t gid = (uint32_t)it.get_global_id(0), nrt = (uint32_t)N / 16u, wk = gid / nrt, rt = gid - wk * nrt;
    const int g0 = (int)(wk * (uint32_t)G) / W, g1 = (int)((wk + 1u) * (uint32_t)G) / W;
    const uint32_t r0 = 16u * rt, lane0 = r0 & 31u, gr = r0 >> 5;
    simd<int64_t, 16> acc[MR];
    unroll<MR>([&](auto mi) { acc[decltype(mi)::value] = 0; });
    auto addr = [=](int s, int c) { return stream + ((size_t)(((uint32_t)s * rows32 + gr) * BITS + c) * 32 + lane0) * 4; };
    simd<uint32_t, 64> nb[BITS];
    if constexpr (PF) unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value; nb[c] = block_load<uint32_t, 64>(addr(2 * g0, c)); });
    const int s_last = 2 * g1 - 1;
    for (int g = g0; g < g1; ++g) {
      simd<int, 16 * D> d[ND];
      unroll<ND>([&](auto di) { d[decltype(di)::value] = 0; });
      for (int h = 0; h < 2; ++h) {
        const int s = 2 * g + h;
        simd<uint32_t, 16> w[NW + 1];
        if constexpr (PF) {
          unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
            unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = nb[c].template select<16, 4>(k); }); });
        } else {
          unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value;
            simd<uint32_t, 64> blk = block_load<uint32_t, 64>(addr(s, c));
            unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; w[c * 4 + k] = blk.template select<16, 4>(k); }); });
        }
        const int sn = s < s_last ? s + 1 : s_last;      // the last trip requests its own blocks again (no branch in the loop body)
        if constexpr (PF == 1) unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value; nb[c] = block_load<uint32_t, 64>(addr(sn, c)); });
        unroll<8>([&](auto chi) { constexpr int ch = decltype(chi)::value;
          if constexpr (PF == 2 && ch == 4) unroll<BITS>([&](auto ci) { constexpr int c = decltype(ci)::value; nb[c] = block_load<uint32_t, 64>(addr(sn, c)); });
          simd<int, 128> B;
          if constexpr (GA == 0) unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
            const simd<uint32_t, 16> x0 = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + 2 * i>(w) * 4u);
            const simd<uint32_t, 16> x1 = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + 2 * i + 1>(w) * 4u);
            B.template select<16, 1>(i * 16) = simd<int, 16>((x0 & 0xffffu) | (x1 << 16u));
          });
          else {      // GA: all 16 lookups of the chunk are requested before the first result is used
            simd<uint32_t, 16> x[16];
            if constexpr (GA == 2) { simd<uint32_t, 16> a[16];
              unroll<16>([&](auto ii) { constexpr int i = decltype(ii)::value; a[i] = state_of<BITS, 16 * ch + i>(w) * 4u; });
              unroll<16>([&](auto ii) { constexpr int i = decltype(ii)::value; x[i] = slm_gather<uint32_t, 16>(a[i]); }); }
            else if constexpr (GA == 3) unroll<16>([&](auto ii) { constexpr int i = decltype(ii)::value;      // every second lookup from a copy of the table in global memory
              if constexpr (i % 2) x[i] = gather<uint32_t, 16>(lut, state_of<BITS, 16 * ch + i>(w) * 4u); else x[i] = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + i>(w) * 4u); });
            else if constexpr (GA == 4) unroll<16>([&](auto ii) { constexpr int i = decltype(ii)::value;      // one in four from global memory
              if constexpr (i % 4 == 3) x[i] = gather<uint32_t, 16>(lut, state_of<BITS, 16 * ch + i>(w) * 4u); else x[i] = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + i>(w) * 4u); });
            else unroll<16>([&](auto ii) { constexpr int i = decltype(ii)::value; x[i] = slm_gather<uint32_t, 16>(state_of<BITS, 16 * ch + i>(w) * 4u); });
            unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
              B.template select<16, 1>(i * 16) = simd<int, 16>((x[2 * i] & 0xffffu) | (x[2 * i + 1] << 16u)); });
          }
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

static std::vector<int64_t> host_ref(int BITS, const std::vector<uint32_t>& stream, const std::vector<uint32_t>& lut, const std::vector<int8_t>& qx,
                                     const std::vector<int32_t>& Aw, int N, int Kc, int MR, int r0) {
  const int G = Kc / 512, rows32 = N / 32, lane0 = r0 & 31, gr = r0 >> 5; std::vector<int64_t> ref((size_t)MR * 16, 0);
  for (int l = 0; l < 16; ++l) for (int g = 0; g < G; ++g) { std::vector<int64_t> dot(MR, 0);
    for (int h = 0; h < 2; ++h) { const int s = 2 * g + h; const size_t gq = (size_t)s * rows32 + gr; uint32_t w[33] = {0};
      for (int c = 0; c < BITS; ++c) for (int k = 0; k < 4; ++k) w[c * 4 + k] = stream[((gq * BITS + c) * 32 + lane0 + l) * 4 + k];
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
  ab::banner(q, "gemm_decode");
  std::vector<std::pair<int, int>> shapes;
  { std::string s = argc > 1 ? argv[1] : "34816x5120,5120x17408,16384x5120,5120x6144"; size_t p = 0;
    while (p < s.size()) { size_t e = s.find(',', p); if (e == std::string::npos) e = s.size(); int n, k;
      if (std::sscanf(s.substr(p, e - p).c_str(), "%dx%d", &n, &k) == 2) shapes.push_back({n, k}); p = e + 1; } }
  const char* be = getenv("ARCH_BITS"); const std::string bsel = be ? be : "6";
  std::printf("shape,bits_per_weight,rows_M,prefetch,gather_ahead,workers,threads,ms_queued,ms_sync,Gweights_per_s,code_GB_per_s,int8_TOPS,check\n");
  for (auto [N, Kc] : shapes) for (int BITS : {6, 8}) {
    if (bsel.find(BITS == 6 ? '6' : '8') == std::string::npos) continue;
    const int G = Kc / 512, MAXM = 16, MAXW = 4; uint64_t seed = 12345 + BITS;
    const size_t nstream = (size_t)N * Kc * BITS / 64;
    std::vector<uint32_t> hs(nstream), hl(4096); std::vector<int8_t> hq((size_t)Kc * MAXM); std::vector<int32_t> ha((size_t)MAXM * G);
    for (auto& v : hs) v = ab::rng(seed);
    for (auto& v : hl) v = ab::rng(seed) & 0xffffu;
    for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    for (auto& v : ha) v = (int32_t)(ab::rng(seed) >> 8);
    Bufs b; b.stream = malloc_device<uint32_t>(nstream, q); b.lut = malloc_device<uint32_t>(4096, q); b.qx = malloc_device<int8_t>(hq.size(), q);
    b.Aw = malloc_device<int32_t>(ha.size(), q); b.out = malloc_device<int64_t>((size_t)MAXW * MAXM * N, q);
    q.memcpy(b.stream, hs.data(), nstream * 4); q.memcpy(b.lut, hl.data(), 16384); q.memcpy(b.qx, hq.data(), hq.size()); q.memcpy(b.Aw, ha.data(), ha.size() * 4); q.wait();
    const double gw = (double)N * Kc / 1e9, gb = (double)nstream * 4 / 1e9;
    auto run = [&]<int B, int MR, int PF, int GA = 0>(int W) {
      if (G % W) return;
      auto launch = [&] { return k_dec<B, MR, PF, GA>(q, b, N, Kc, W); };
      launch().wait(); const char* c = "exact";
      { std::vector<int64_t> dv((size_t)W * MR * N); q.memcpy(dv.data(), b.out, dv.size() * 8).wait();
        for (int r0 : {0, (N / 2) & ~15}) { auto ref = host_ref(B, hs, hl, hq, ha, N, Kc, MR, r0);
          for (int m = 0; m < MR; ++m) for (int l = 0; l < 16; ++l) { int64_t s = 0; for (int w = 0; w < W; ++w) s += dv[((size_t)w * MR + m) * N + r0 + l]; if (s != ref[m * 16 + l]) c = "MISMATCH"; } } }
      const double tq = ab::best_time(q, launch, false), ts = ab::best_time(q, launch, true);
      std::printf("%dx%d,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.1f,%.1f,%.2f,%s\n", N, Kc, B / 2, MR, PF, GA, W, N / 16 * W, tq * 1e3, ts * 1e3, gw / tq, gb / tq, gw * MR * 2 / tq / 1e3, c);
      std::fflush(stdout); };
    auto all = [&]<int B>() {
      for (int W : {1, 2, 4}) { if (W > 1 && N > 8192) continue;
#define ROW(MR) run.template operator()<B, MR, 0>(W); run.template operator()<B, MR, 1>(W); run.template operator()<B, MR, 2>(W); \
  run.template operator()<B, MR, 0, 1>(W); run.template operator()<B, MR, 0, 2>(W); run.template operator()<B, MR, 0, 3>(W); run.template operator()<B, MR, 0, 4>(W);
        ROW(1) ROW(2) ROW(4) ROW(8) ROW(16)
#undef ROW
      } };
    if (BITS == 6) all.template operator()<6>(); else all.template operator()<8>();
    free(b.stream, q); free(b.lut, q); free(b.qx, q); free(b.Aw, q); free(b.out, q);
  }
  return 0;
}
