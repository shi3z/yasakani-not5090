// SPDX-License-Identifier: Apache-2.0
// Tensor-core (mma.sync) throughput and latency on CUDA (mirror of b60/xmx.cpp).
//  reg : register-only loops of mma.sync m16n8k32 s8 and m16n8k16 bf16.  NCH = number of independent accumulators:
//        NCH = 1 is a dependent chain (latency), NCH = 4 independent streams (throughput).  Swept over the number of warps.
//        One mma = 16 x 8 x 32 = 4096 int8 (16 x 8 x 16 = 2048 bf16) multiply-accumulates = 8 dpas "rows" of the B60 program,
//        so the dpas-row columns are filled with 8 rows per mma.  add32 = the same loop with one 32-bit add (inline PTX, not foldable).
//  gemm: uncompressed int8 (or bf16) GEMM, Y[M][N] = X[M][K] * W[N][K]^T.  One warp = 16 output rows (A operand = weights)
//        x 8*NB input rows (NB B operands of 8 input rows); M = RS * 8 * NB (M < 8: the unused input rows are computed and dropped).
//        W uint4 [N/16][K/32][32 lanes] (bf16: K/16), X uint2 [K/32][ceil(M/8)][32 lanes]: both in register order, one load per lane.
// usage: mma      env ARCH_ONLY=reg,gemm
#include "common.cuh"

__device__ __forceinline__ void mma_s8(int32_t* d, const uint32_t* a, const uint32_t* b) {
  asm volatile("mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
               : "+r"(d[0]), "+r"(d[1]), "+r"(d[2]), "+r"(d[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
}
__device__ __forceinline__ void mma_bf16(float* d, const uint32_t* a, const uint32_t* b) {
  asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
               : "+f"(d[0]), "+f"(d[1]), "+f"(d[2]), "+f"(d[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
}
__device__ __forceinline__ void add_s32(int32_t& d, int32_t b) { asm volatile("add.s32 %0, %0, %1;\n" : "+r"(d) : "r"(b)); }

// KIND 0 add32, 1 mma s8, 2 mma bf16.  src: 8 dwords per lane (4 of A, 2 of B).
template <int NCH, int KIND>
__global__ void k_reg(const uint32_t* src, float* out, int iters) {
  const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x, lane = threadIdx.x & 31u;
  uint32_t a[4], b[2];
  #pragma unroll
  for (int i = 0; i < 4; ++i) a[i] = src[lane * 8 + i];
  #pragma unroll
  for (int i = 0; i < 2; ++i) b[i] = src[lane * 8 + 4 + i];
  float s = 0.f;
  if constexpr (KIND == 0) {
    int32_t acc[NCH];
    #pragma unroll
    for (int k = 0; k < NCH; ++k) acc[k] = 0;
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) add_s32(acc[k], (int32_t)b[0]);
    }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += (float)acc[k];
  } else if constexpr (KIND == 1) {
    int32_t acc[NCH][4];
    #pragma unroll
    for (int k = 0; k < NCH; ++k) acc[k][0] = acc[k][1] = acc[k][2] = acc[k][3] = 0;
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) mma_s8(acc[k], a, b);
    }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += (float)acc[k][0] + (float)acc[k][1] + (float)acc[k][2] + (float)acc[k][3];
  } else {
    float acc[NCH][4];
    #pragma unroll
    for (int k = 0; k < NCH; ++k) acc[k][0] = acc[k][1] = acc[k][2] = acc[k][3] = 0.f;
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) mma_bf16(acc[k], a, b);
    }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += acc[k][0] + acc[k][1] + acc[k][2] + acc[k][3];
  }
  out[gid] = s;
}

// A registers of lane (r = lane / 4, g = lane % 4), int8: {row r, k 4g..4g+3}, {row r+8, same}, {row r, k 16+4g..}, {row r+8, same}
//                                                   bf16: {row r, k 2g, 2g+1}, {row r+8, same}, {row r, k 8+2g..}, {row r+8, same}
// B registers: int8 {k 4g..4g+3, col r}, {k 16+4g.., col r};  bf16 {k 2g, 2g+1, col r}, {k 8+2g.., col r}
// D registers: {row r, col 2g}, {row r, col 2g+1}, {row r+8, col 2g}, {row r+8, col 2g+1}.   row = output row n, col = input row m.
template <int NB, bool BF>
__global__ void __launch_bounds__(128) k_gemm(const uint4* W, const uint2* X, void* Y, int N, int nchunk, int M) {
  const int warp = threadIdx.x >> 5, lane = threadIdx.x & 31, r = lane >> 2, g = lane & 3;
  const int tile = blockIdx.x * 4 + warp, MB = (M + 7) / 8, tb0 = blockIdx.y * NB;
  if (tile >= N / 16) return;
  int32_t ai[NB][4]; float af[NB][4];
  #pragma unroll
  for (int nb = 0; nb < NB; ++nb)
    #pragma unroll
    for (int q = 0; q < 4; ++q) { ai[nb][q] = 0; af[nb][q] = 0.f; }
  const uint4* w = W + (size_t)tile * nchunk * 32 + lane;
  for (int c = 0; c < nchunk; ++c) {
    const uint4 a4 = w[(size_t)c * 32]; const uint32_t a[4] = {a4.x, a4.y, a4.z, a4.w};
    const uint2* xa = X + ((size_t)c * MB + tb0) * 32 + lane;
    #pragma unroll
    for (int nb = 0; nb < NB; ++nb) { const uint2 b2 = xa[nb * 32]; const uint32_t b[2] = {b2.x, b2.y};
      if constexpr (BF) mma_bf16(af[nb], a, b); else mma_s8(ai[nb], a, b); }
  }
  #pragma unroll
  for (int nb = 0; nb < NB; ++nb)
    #pragma unroll
    for (int q = 0; q < 4; ++q) { const int tok = (tb0 + nb) * 8 + 2 * g + (q & 1), n = tile * 16 + r + 8 * (q >> 1);
      if (tok < M) { if constexpr (BF) static_cast<float*>(Y)[(size_t)tok * N + n] = af[nb][q]; else static_cast<int32_t*>(Y)[(size_t)tok * N + n] = ai[nb][q]; } }
}

static uint16_t f2bf(float f) { uint32_t u; memcpy(&u, &f, 4); u += 0x7fffu + ((u >> 16) & 1u); return (uint16_t)(u >> 16); }

// Host accessors of the int8 layouts.
static int w8(const std::vector<uint32_t>& hw, int nchunk, int n, int k) {
  const int tile = n / 16, rr = n % 16, r = rr & 7, hi = rr >> 3, c = k / 32, kk = k % 32, half = kk >> 4, g = (kk & 15) >> 2, e = kk & 3;
  return (int8_t)(hw[(((size_t)tile * nchunk + c) * 32 + r * 4 + g) * 4 + half * 2 + hi] >> (8 * e));
}
static int x8(const std::vector<uint32_t>& hx, int MB, int m, int k) {
  const int tb = m >> 3, r = m & 7, c = k / 32, kk = k % 32, half = kk >> 4, g = (kk & 15) >> 2, e = kk & 3;
  return (int8_t)(hx[(((size_t)c * MB + tb) * 32 + r * 4 + g) * 2 + half] >> (8 * e));
}

struct Gemm { const uint4* W; const uint2* X; void* Y; const std::vector<uint32_t>* hw; const std::vector<uint32_t>* hx; int N, K, nchunk; size_t wbytes; };
template <int NB, bool BF> static void gemm(const Gemm& c, int M) {
  const int N = c.N, RS = (M + 7) / 8 / NB, warps = N / 16 * RS;
  auto launch = [&] { k_gemm<NB, BF><<<dim3((N / 16 + 3) / 4, RS), 128>>>(c.W, c.X, c.Y, N, c.nchunk, M); };
  const char* chk = "-";
  if (!BF) {            // exact check of the first and the last 16 output rows
    launch(); ab::launched(); ab::sync(); const std::vector<int32_t> y = ab::download<int32_t>(c.Y, (size_t)M * N); chk = "exact";
    for (int n0 : {0, N - 16}) for (int m = 0; m < M; ++m) for (int i = 0; i < 16; ++i) { int s = 0;
      for (int k = 0; k < c.K; ++k) s += w8(*c.hw, c.nchunk, n0 + i, k) * x8(*c.hx, (M + 7) / 8, m, k);
      if (s != y[(size_t)m * N + n0 + i]) chk = "MISMATCH"; }
  }
  const double tq = ab::best_time(launch, false), ts = ab::best_time(launch, true);
  std::printf("gemm,%s,%d,%d,%d,%d,%d,%.4f,%.4f,%.2f,%.1f,%s,%d\n", BF ? "bf16" : "s8", N, c.K, M, 8 * NB, warps, tq * 1e3, ts * 1e3,
              2.0 * N * c.K * M / tq / 1e12, (double)c.wbytes / tq / 1e9, chk, (N / 16 + 3) / 4 * RS);
  std::fflush(stdout);
}
template <bool BF> static void gemm_all(const Gemm& c) {
  gemm<1, BF>(c, 1); gemm<1, BF>(c, 4); gemm<1, BF>(c, 8); gemm<2, BF>(c, 16);
  gemm<4, BF>(c, 32); gemm<2, BF>(c, 32);
  gemm<8, BF>(c, 64); gemm<4, BF>(c, 64); gemm<2, BF>(c, 64); gemm<1, BF>(c, 64);
}

struct Reg { const uint32_t* src; float* out; std::vector<int> warps; };
template <int NCH, int KIND> static void reg(const Reg& c, const char* name, int iters, double macs) {
  const int D = KIND == 0 ? 1 : 8;               // dpas-row equivalents per operation
  for (int w : c.warps) { const int bt = std::min(w, 4) * 32, blocks = (w + 3) / 4;
    const double s = ab::best_time([&] { k_reg<NCH, KIND><<<blocks, bt>>>(c.src, c.out, iters); }, true, 0.03, 4);
    const double ops = (double)w * iters * NCH, rows = ops * D;
    std::printf("reg,%s,%d,%d,%d,%d,%.4f,%.3f,%.4g,%.2f,%d,%.3f,%.4g\n", name, D, NCH, w, iters, s * 1e3, s * 1e9 / ((double)iters * NCH * D), rows / s,
                rows / s * macs * 2 / 1e12, blocks, s * 1e9 / ((double)iters * NCH), ops / s);
    std::fflush(stdout); }
}

int main() {
  ab::banner("mma");
  uint64_t seed = 99;
  if (ab::want("reg")) {
    std::vector<uint32_t> hs(256); for (auto& v : hs) v = ab::rng(seed) & 0x03030303u;          // small int8 values
    std::vector<uint32_t> hb(256); for (auto& v : hb) { const uint32_t r = ab::rng(seed); v = f2bf((float)(r >> 24) / 4096.f) | ((uint32_t)f2bf((float)(r & 255) / 4096.f) << 16); }
    Reg c; const int sm = ab::prop().multiProcessorCount;
    c.warps = {1, 2, 4, 8, 16, 32};
    for (int k : {1, 2, 4, 8, 12, 16, 24, 32, 40, 48, 64, 96, 192}) { const int w = (sm * k + 3) & ~3; if (w > c.warps.back()) c.warps.push_back(w); }   // 48 warps per SM = all resident
    uint32_t* src = ab::upload(hs); uint32_t* srcb = ab::upload(hb); c.out = ab::dalloc<float>((size_t)c.warps.back() * 32);
    std::printf("section,kernel,rows_per_dpas,chains,threads,iters,ms,ns_per_dpas_row_in_thread,dpas_rows_per_s,TOPS_or_TFLOPS,blocks,ns_per_op_in_warp,ops_per_s\n");
    std::printf("# CUDA: threads = warps; one mma.sync = 8 dpas rows (4096 int8 / 2048 bf16 multiply-accumulates); add32: one op = 32 lane adds\n");
    c.src = src;
    reg<1, 0>(c, "add32", 200000, 32); reg<4, 0>(c, "add32", 100000, 32);
    reg<1, 1>(c, "mma_s8_m16n8k32", 100000, 512); reg<4, 1>(c, "mma_s8_m16n8k32", 50000, 512);
    c.src = srcb;
    reg<1, 2>(c, "mma_bf16_m16n8k16", 100000, 256); reg<4, 2>(c, "mma_bf16_m16n8k16", 50000, 256);
    cudaFree(src); cudaFree(srcb); cudaFree(c.out);
  }
  if (ab::want("gemm")) {
    std::printf("section,type,N,K,rows_M,rows_per_thread,threads,ms_queued,ms_sync,TOPS_or_TFLOPS,weight_GB_per_s,check,blocks\n");
    std::printf("# CUDA: threads = warps, rows_per_thread = input rows per warp\n");
    const int shapes[][2] = {{17408, 5120}, {5120, 17408}, {20480, 512}, {81920, 512}};
    for (auto& sh : shapes) for (int bf = 0; bf < 2; ++bf) {
      const int N = sh[0], K = sh[1], cols = bf ? 16 : 32, nchunk = K / cols, MAXM = 64;
      const size_t nw = (size_t)(N / 16) * nchunk * 128, nx = (size_t)nchunk * MAXM * 8;        // dwords
      std::vector<uint32_t> hw(nw), hx(nx);
      auto bfpair = [&] { const uint32_t r = ab::rng(seed); return (uint32_t)f2bf((float)(r & 255) / 256.f - 0.5f) | ((uint32_t)f2bf((float)(r >> 24) / 256.f - 0.5f) << 16); };
      if (bf) { for (auto& v : hw) v = bfpair(); for (auto& v : hx) v = bfpair(); }
      else { for (auto& v : hw) v = ab::rng(seed); for (auto& v : hx) v = ab::rng(seed); }
      uint32_t* W = ab::upload(hw); uint32_t* X = ab::upload(hx); int32_t* Y = ab::dalloc<int32_t>((size_t)MAXM * N);
      const Gemm c{reinterpret_cast<const uint4*>(W), reinterpret_cast<const uint2*>(X), Y, &hw, &hx, N, K, nchunk, nw * 4};
      if (bf) gemm_all<true>(c); else gemm_all<false>(c);
      cudaFree(W); cudaFree(X); cudaFree(Y);
    }
  }
  return 0;
}
