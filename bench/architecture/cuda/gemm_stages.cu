// SPDX-License-Identifier: Apache-2.0
// Stage decomposition of the compressed GEMM on CUDA (mirror of b60/gemm_stages.cpp; kernel and layouts in gemm_kernel.cuh).
// Stages (each includes the previous ones):
//   A read the packed codes (global -> the warp's shared-memory stage)   B + extract the 12-bit states (funnel shifts)
//   C + value-table lookup (shared memory)   D + build the int8 B fragment   E + mma.sync m16n8k32 (int32 sums)
//   F + int64 accumulation per 512 columns   G + scale to float
// Structure: block = 4 warps, warp = 16 output rows x 16*MT input rows; MT = 1 for M <= 16, MT = 2 for M = 32, 64 (M = 64: two blocks
// per tile, the codes are decoded twice).  Stages A..D are run with M = 1 only (they do not touch the input rows).
// usage: gemm_stages [shapes]     shapes = comma list of NxKc, default 34816x5120,5120x17408,20480x17408   (N % 64 == 0, Kc % 512 == 0)
//        env ARCH_ONLY=small,big (M <= 16 / M >= 32)   ARCH_BITS=6,8
#include "gemm_kernel.cuh"

struct Ctx { gk::Args P; int N, Kc, K; const std::vector<uint32_t>* hs; const std::vector<uint16_t>* hl; const std::vector<int8_t>* hq; const std::vector<int32_t>* ha;
             double gw, gb; };
static const char* stn[] = {"A_read", "B_states", "C_lookup", "D_operand", "E_mma", "F_int64", "G_float"};

// Exact check of stage F against the host for three tiles (48 output rows), all M input rows.
static const char* check(const Ctx& c, int M) {
  const int N = c.N; const std::vector<int64_t> dv = ab::download<int64_t>(c.P.out, (size_t)M * N);
  for (int n0 : {0, (N / 2) & ~15, N - 16}) { const std::vector<int64_t> ref = gk::host_ref(c.K, *c.hs, *c.hl, *c.hq, c.ha->data(), N, c.Kc, M, n0, 16);
    for (int m = 0; m < M; ++m) for (int i = 0; i < 16; ++i) if (dv[(size_t)m * N + n0 + i] != ref[(size_t)m * 16 + i]) return "MISMATCH"; }
  return "exact";
}
template <int K, int MT, int ST> static void run(Ctx& c, int M) {
  gk::Args P = c.P; P.M = M;
  auto launch = [&] { gk::launch<K, MT, ST, 0>(P); };
  const char* chk = "-";
  if (ST == 5) { launch(); ab::launched(); ab::sync(); chk = check(c, M); }
  const gk::Grid gr = gk::grid_of<K, MT, ST, 0>(P);
  const double tq = ab::best_time(launch, false), ts = ab::best_time(launch, true);
  std::printf("%dx%d,%d,warp,%d,%d,%s,%d,%.4f,%.4f,%.1f,%.1f,%s,%d,%d,%d\n", c.N, c.Kc, K / 2, M, 16 * MT, stn[ST], gr.warps, tq * 1e3, ts * 1e3,
              c.gw / tq, c.gb / tq, chk, gr.bx * gr.by, gr.bx * gr.by * 32 * gk::WPB, gr.smem);
  std::fflush(stdout);
}
template <int K> static void all(Ctx& c) {
  if (ab::want("small")) {
    run<K, 1, 0>(c, 1); run<K, 1, 1>(c, 1); run<K, 1, 2>(c, 1); run<K, 1, 3>(c, 1); run<K, 1, 4>(c, 1); run<K, 1, 5>(c, 1); run<K, 1, 6>(c, 1);
    for (int M : {8, 16}) { run<K, 1, 4>(c, M); run<K, 1, 5>(c, M); run<K, 1, 6>(c, M); }
  }
  if (ab::want("big")) for (int M : {32, 64}) { run<K, 2, 4>(c, M); run<K, 2, 5>(c, M); run<K, 2, 6>(c, M); }
}

int main(int argc, char** argv) {
  ab::banner("gemm_stages");
  std::vector<std::pair<int, int>> shapes;
  { std::string s = argc > 1 ? argv[1] : "34816x5120,5120x17408,20480x17408"; size_t p = 0;
    while (p < s.size()) { size_t e = s.find(',', p); if (e == std::string::npos) e = s.size(); int n, k;
      if (std::sscanf(s.substr(p, e - p).c_str(), "%dx%d", &n, &k) == 2) shapes.push_back({n, k}); p = e + 1; } }
  const char* be = getenv("ARCH_BITS"); const std::string bsel = be ? be : "6,8";
  std::printf("shape,bits_per_weight,structure,rows_M,rows_per_thread,stage,threads,ms_queued,ms_sync,Gweights_per_s,code_GB_per_s,check,blocks,cuda_threads,smem_bytes_per_block\n");
  for (auto [N, Kc] : shapes) for (int K : {6, 8}) {
    if (bsel.find(K == 6 ? '6' : '8') == std::string::npos) continue;
    if (N % 64 || Kc % 512) { std::printf("# skipped %dx%d: N must be a multiple of 64 and Kc of 512\n", N, Kc); continue; }
    const int G = Kc / 512, MAXM = 64; uint64_t seed = 12345 + K;
    const size_t nstream = (size_t)N * Kc * K / 64;         // dwords (K / 16 bytes per weight)
    std::vector<uint32_t> hs(nstream); std::vector<uint16_t> hl(4096); std::vector<int8_t> hq((size_t)Kc * MAXM); std::vector<int32_t> ha((size_t)MAXM * G);
    for (auto& v : hs) v = ab::rng(seed);
    for (auto& v : hl) v = (uint16_t)ab::rng(seed);
    for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    for (auto& v : ha) v = (int32_t)(ab::rng(seed) >> 8);
    std::vector<float> hu(MAXM, 1.0f / 16777216.f), ht(N, 0.37f);
    Ctx c; c.N = N; c.Kc = Kc; c.K = K; c.hs = &hs; c.hl = &hl; c.hq = &hq; c.ha = &ha; c.gw = (double)N * Kc / 1e9; c.gb = (double)nstream * 4 / 1e9;
    uint32_t* dstream = ab::upload(hs); uint16_t* dlut = ab::upload(hl); int8_t* dqx = ab::upload(hq); int32_t* daw = ab::upload(ha);
    float* dus = ab::upload(hu); float* dtau = ab::upload(ht); long long* dout = ab::dalloc<long long>((size_t)MAXM * N);
    c.P = gk::Args{reinterpret_cast<const uint4*>(dstream), dlut, nullptr, nullptr, dqx, daw, dus, dtau, dout, N, Kc, 1};
    if (K == 6) all<6>(c); else all<8>(c);
    cudaFree(dstream); cudaFree(dlut); cudaFree(dqx); cudaFree(daw); cudaFree(dus); cudaFree(dtau); cudaFree(dout);
  }
  return 0;
}
