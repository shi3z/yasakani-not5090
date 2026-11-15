// SPDX-License-Identifier: Apache-2.0
// "Decode once" compressed GEMM for many input rows (prompt reading), an experiment for the B60.
// A work-group of 16 threads owns 16 output rows.  For every 512-column group each thread decodes ONE of the 16 column chunks
// (32 columns x 16 rows = one dpas weight operand, 16 SLM gathers) and stores it in SLM; after a barrier every thread
// multiplies all 16 operands with its OWN set of D*ND input rows.  The weights are decoded once per launch for
// M = 16 * D * ND input rows (the engine's prefill kernel decodes them once per 32 rows).
// Result: exact int64 sums, part[m][n] = sum_g Aw[m][g] * sum_{c in g} w8(n, c) * qx[m][c].
// Code layout needed (a load-time permutation of the file's stream): [Kc/256][N/32][4*BITS words][32 lanes], so that a
// thread can load just the BITS/2 + 1 words of its chunk.
// usage: gemm_once [shapes]      default 34816x5120,5120x17408      env ARCH_BITS=6,8  ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
constexpr int LS = 16;
struct Bufs { uint32_t* stream; uint32_t* lut; int8_t* qx; int32_t* Aw; int64_t* out; };

// AM: where the exact int64 sums live between the column groups.  0 = in `part` itself (global memory, read-modify-write per group,
// as the engine's prefill kernel), 1 = in registers (2 registers per input row), 2 = in SLM (128 bytes per input row and thread).
template <int BITS, int D, int ND, int AM>
event k_once(queue& q, Bufs b, int N, int Kc) {
  constexpr int NW = 4 * BITS, WPC = BITS / 2, MR = LS * D * ND, RPT = D * ND, ACC0 = 16384 + 16 * 512;
  const int G = Kc / 512, rows32 = N / 32;
  const uint32_t* stream = b.stream; const uint32_t* lut = b.lut; const int8_t* qx = b.qx; const int32_t* Aw = b.Aw; int64_t* part = b.out;
  return q.parallel_for(nd_range<1>((size_t)(N / 16) * LS, LS), [=](nd_item<1> it) SYCL_ESIMD_KERNEL {
    slm_init<ACC0 + (AM == 2 ? LS * RPT * 128 : 0)>();
    const uint32_t lid = it.get_local_id(0);
    for (uint32_t i = lid; i < 64; i += LS) slm_block_store<uint32_t, 64>(i * 256, block_load<uint32_t, 64>(lut + i * 64));
    barrier();
    const uint32_t r0 = 16u * (uint32_t)it.get_group(0), lane0 = r0 & 31u, gr = r0 >> 5;
    const int h = (int)lid >> 3, ch = (int)lid & 7, m0 = (int)lid * D * ND;
    simd<int64_t, 16> acc[AM == 1 ? RPT : 1];
    if constexpr (AM == 1) unroll<RPT>([&](auto ri) { acc[decltype(ri)::value] = 0; });
    for (int g = 0; g < G; ++g) {
      { // decode chunk (h, ch) of this group: states 16*ch .. 16*ch+15 of half block s = 2g + h
        const uint32_t gq = (uint32_t)(2 * g + h) * rows32 + gr;
        const uint32_t* wp = stream + ((size_t)gq * NW + WPC * ch) * 32 + lane0;       // word WPC*ch of the 16 lanes
        simd<uint32_t, 16> wv[WPC + 1];
        wv[0] = 0; if (ch) wv[0] = block_load<uint32_t, 16>(wp - 32);                   // the bits below the chunk (zero for chunk 0)
        unroll<WPC>([&](auto ki) { constexpr int k = decltype(ki)::value; wv[k + 1] = block_load<uint32_t, 16>(wp + k * 32); });
        simd<uint32_t, 128> B;
        unroll<8>([&](auto ii) { constexpr int i = decltype(ii)::value;
          simd<uint32_t, 16> gth[2];
          unroll<2>([&](auto hi) { constexpr int hh = decltype(hi)::value;
            constexpr int p = 32 + BITS * (2 * i + hh + 1) - 14, wi = p >> 5, sh = p & 31;    // SLM byte offset = state * 4
            if constexpr (sh + 14 <= 32) gth[hh] = slm_gather<uint32_t, 16>((wv[wi] >> (uint32_t)sh) & 0x3ffcu);
            else gth[hh] = slm_gather<uint32_t, 16>(((wv[wi] >> (uint32_t)sh) | (wv[wi + 1] << (uint32_t)(32 - sh))) & 0x3ffcu);
          });
          B.template select<16, 1>(i * 16) = (gth[0] & 0xffffu) | (gth[1] << 16u);
        });
        slm_block_store<uint32_t, 64>(16384 + lid * 512, B.template select<64, 1>(0));
        slm_block_store<uint32_t, 64>(16384 + lid * 512 + 256, B.template select<64, 1>(64));
      }
      barrier();
      simd<int, 16 * D> d[ND];
      unroll<ND>([&](auto di) { d[decltype(di)::value] = 0; });
      unroll<16>([&](auto ci) { constexpr int c = decltype(ci)::value;
        simd<int, 128> B; B.template select<64, 1>(0) = slm_block_load<int, 64>(16384 + c * 512); B.template select<64, 1>(64) = slm_block_load<int, 64>(16384 + c * 512 + 256);
        const int8_t* xa = qx + ((size_t)(g * 16 + c) * MR + m0) * 32;
        unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value;
          const simd<int, 8 * D> A = block_load<int, 8 * D>((const int*)(xa + dd * D * 32));
          d[dd] = xmx::dpas<8, D, int, int, int, int, xmx::dpas_argument_type::s8, xmx::dpas_argument_type::s8>(d[dd], B, A); });
      });
      unroll<ND>([&](auto di) { constexpr int dd = decltype(di)::value; unroll<D>([&](auto mi) { constexpr int m = decltype(mi)::value;
        const int row = m0 + dd * D + m; int64_t* pp = part + (size_t)row * N + r0;
        simd<int64_t, 16> wide = d[dd].template select<16, 1>(16 * m); wide = wide * (int64_t)Aw[row * G + g];
        if constexpr (AM == 0) { if (g) wide += block_load<int64_t, 16>(pp); block_store<int64_t, 16>(pp, wide); }
        else if constexpr (AM == 1) acc[dd * D + m] += wide;
        else { const uint32_t so = ACC0 + (lid * RPT + dd * D + m) * 128;
          if (g) wide += slm_block_load<int64_t, 16>(so);
          if (g == G - 1) block_store<int64_t, 16>(pp, wide); else slm_block_store<int64_t, 16>(so, wide); } }); });
      barrier();
    }
    if constexpr (AM == 1) unroll<RPT>([&](auto ri) { constexpr int r = decltype(ri)::value; block_store<int64_t, 16>(part + (size_t)(m0 + r) * N + r0, acc[r]); });
  });
}

static std::vector<int64_t> host_ref(int BITS, const std::vector<uint32_t>& stream, const std::vector<uint32_t>& lut, const std::vector<int8_t>& qx,
                                     const std::vector<int32_t>& Aw, int N, int Kc, int MR, int r0, const std::vector<int>& rows) {
  const int G = Kc / 512, rows32 = N / 32, lane0 = r0 & 31, gr = r0 >> 5, NW = 4 * BITS; std::vector<int64_t> ref(rows.size() * 16, 0);
  for (int l = 0; l < 16; ++l) for (int g = 0; g < G; ++g) { std::vector<int64_t> dot(rows.size(), 0);
    for (int h = 0; h < 2; ++h) { const int s = 2 * g + h; const size_t gq = (size_t)s * rows32 + gr; uint32_t w[34] = {0};
      for (int k = 0; k < NW; ++k) w[k] = stream[(gq * NW + k) * 32 + lane0 + l];
      for (int j = 0; j < 128; ++j) { const int a = BITS * (j + 1) - 12; uint32_t st;
        if (a < 0) st = (w[0] << (-a)) & 0xfffu; else { const int wi = a >> 5, sh = a & 31; uint64_t v = w[wi] | ((uint64_t)w[wi + 1] << 32); st = (uint32_t)(v >> sh) & 0xfffu; }
        for (int e = 0; e < 2; ++e) { const int wv = (int8_t)(lut[st] >> (8 * e)), col = 2 * j + e;
          for (size_t mi = 0; mi < rows.size(); ++mi) dot[mi] += wv * (int)qx[((size_t)(s * 8 + col / 32) * MR + rows[mi]) * 32 + col % 32]; } } }
    for (size_t mi = 0; mi < rows.size(); ++mi) ref[mi * 16 + l] += (int64_t)(int32_t)dot[mi] * Aw[rows[mi] * G + g]; }
  return ref;
}

int main(int argc, char** argv) {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "gemm_once");
  std::vector<std::pair<int, int>> shapes;
  { std::string s = argc > 1 ? argv[1] : "34816x5120,5120x17408"; size_t p = 0;
    while (p < s.size()) { size_t e = s.find(',', p); if (e == std::string::npos) e = s.size(); int n, k;
      if (std::sscanf(s.substr(p, e - p).c_str(), "%dx%d", &n, &k) == 2) shapes.push_back({n, k}); p = e + 1; } }
  const char* be = getenv("ARCH_BITS"); const std::string bsel = be ? be : "6,8";
  std::printf("shape,bits_per_weight,structure,rows_M,rows_per_thread,stage,threads,ms_queued,ms_sync,Gweights_per_s,code_GB_per_s,check,G_int8_MAC_per_s\n");
  for (auto [N, Kc] : shapes) for (int BITS : {6, 8}) {
    if (bsel.find(BITS == 6 ? '6' : '8') == std::string::npos) continue;
    const int G = Kc / 512, MAXM = 512; uint64_t seed = 4242 + BITS;
    const size_t nstream = (size_t)N * Kc * BITS / 64;
    std::vector<uint32_t> hs(nstream), hl(4096); std::vector<int8_t> hq((size_t)Kc * MAXM); std::vector<int32_t> ha((size_t)MAXM * G);
    for (auto& v : hs) v = ab::rng(seed);
    for (auto& v : hl) v = ab::rng(seed) & 0xffffu;
    for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    for (auto& v : ha) v = (int32_t)(ab::rng(seed) >> 8);
    Bufs b; b.stream = malloc_device<uint32_t>(nstream, q); b.lut = malloc_device<uint32_t>(4096, q); b.qx = malloc_device<int8_t>(hq.size(), q);
    b.Aw = malloc_device<int32_t>(ha.size(), q); b.out = malloc_device<int64_t>((size_t)MAXM * N, q);
    q.memcpy(b.stream, hs.data(), nstream * 4); q.memcpy(b.lut, hl.data(), 16384); q.memcpy(b.qx, hq.data(), hq.size()); q.memcpy(b.Aw, ha.data(), ha.size() * 4); q.wait();
    auto run = [&]<int B, int D, int ND, int AM = 0>() {
      const int M = LS * D * ND;
      auto launch = [&] { return k_once<B, D, ND, AM>(q, b, N, Kc); };
      q.memset(b.out, 0xff, (size_t)M * N * 8).wait();      // stale results of another variant must not pass the check
      launch().wait(); const char* chk = "exact";
      const std::vector<int> rows = {0, D * ND - 1, D * ND, M / 2 + 1, M - 1};
      for (int r0 : {0, (N / 2) & ~15}) { auto ref = host_ref(BITS, hs, hl, hq, ha, N, Kc, M, r0, rows); std::vector<int64_t> dv(16);
        for (size_t mi = 0; mi < rows.size(); ++mi) { q.memcpy(dv.data(), b.out + (size_t)rows[mi] * N + r0, 128).wait();
          for (int l = 0; l < 16; ++l) if (dv[l] != ref[mi * 16 + l]) chk = "MISMATCH"; } }
      const double tq = ab::best_time(q, launch, false), ts = ab::best_time(q, launch, true);
      std::printf("%dx%d,%d,%s,%d,%d,F_int64,%d,%.4f,%.4f,%.1f,%.1f,%s,%.0f\n", N, Kc, BITS / 2, AM == 0 ? "decode_once_sums_in_memory" : AM == 1 ? "decode_once_sums_in_registers" : "decode_once_sums_in_slm", M, D * ND, N / 16 * LS, tq * 1e3, ts * 1e3,
                  (double)N * Kc / tq / 1e9, (double)nstream * 4 / tq / 1e9, chk, (double)N * Kc * M / tq / 1e9);
      std::fflush(stdout); };
    auto all = [&]<int B>() { run.template operator()<B, 4, 1>(); run.template operator()<B, 8, 1>(); run.template operator()<B, 8, 2>(); run.template operator()<B, 8, 4>();
      run.template operator()<B, 4, 1, 1>(); run.template operator()<B, 8, 1, 1>(); run.template operator()<B, 8, 2, 1>();
      run.template operator()<B, 4, 1, 2>(); run.template operator()<B, 8, 1, 2>(); run.template operator()<B, 8, 2, 2>(); run.template operator()<B, 8, 4, 2>(); };
    if (BITS == 6) all.template operator()<6>(); else all.template operator()<8>();
    free(b.stream, q); free(b.lut, q); free(b.qx, q); free(b.Aw, q); free(b.out, q);
  }
  return 0;
}
