// SPDX-License-Identifier: Apache-2.0
// Compressed GEMM kernel used by gemm_stages.cu and lut_variants.cu (synthetic layout, same per-weight work as the engine's CUDA GEMM).
//   acc[m][n] = sum over 512-column groups g of Aw[m][g] * sum_{c in g} w8(n, c) * qx[m][c]      (exact int64)
// One block = 4 warps.  One warp = a tile of 16 output rows (NT = 2 units of 8 rows) x 16*MT input rows, all Kc columns.
// Per 256-column half ("ks"): the warp copies its 16 rows of codes (16 bytes per lane and load) into its own shared-memory stage,
// each lane (r = lane / 4 = output row in the unit, g = lane % 4 = quarter of the 256 columns) funnel-shifts its 32 states into
// registers, and 8 mma.sync m16n8k32 per unit and per 16 input rows consume them (B = 8 output rows x 32 columns, A = 16 input rows).
// Layouts:
//   frag  uint4 [N/16 tiles][Kc/256][NT][K][8 rows]   row bit stream of a half = K * 128 bits, LSB first; state J = bits [K(J+1)-12, K(J+1))
//   lut   uint16[4096]   low byte = weight of column 2J, high byte = column 2J+1    (in shared memory: every entry twice, as the engine)
//   qx    int8 [M][Kc],  Aw int32 [M][G],  us float [M],  tau float [N]
//   out   stage A..D: uint32 [N/16][32] checksums; E: int32 [M][N]; F: int64 [M][N]; G: float [M][N]
#pragma once
#include "common.cuh"
namespace gk {
constexpr int L_BITS = 12;
constexpr int NT = 2, WPB = 4, LDA = 256 + 16, REP = 2;     // units per warp, warps per block, pitch of an input row in shared memory, table copies
struct Args { const uint4* frag; const uint16_t* lut; const uint32_t* glut; const uint32_t* glut18; const int8_t* qx; const int32_t* Aw;
              const float* us; const float* tau; void* out; int N, Kc, M; };

__device__ __forceinline__ void mma_s8_16832(int32_t* d, const uint32_t* a, uint2 b) {
  asm volatile("mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
               : "+r"(d[0]), "+r"(d[1]), "+r"(d[2]), "+r"(d[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b.x), "r"(b.y));
}

// The bits of one row that a lane needs: 32 states starting at state 32 * g, i.e. row bits [32 g K - 12, 32 (g + 1) K).
template <int K> struct UnitQ { static constexpr int NW = (32 * K + L_BITS + 31) / 32; uint32_t w[NW]; };
template <int K> __device__ __forceinline__ void unit_load(UnitQ<K>& u, const unsigned char* row, int g) {
  constexpr int WPS = 4 * K;                                    // dwords of a row
  const uint32_t* S = reinterpret_cast<const uint32_t*>(row);
  const int off = 32 * g * K - L_BITS, q = off >> 5, sh = off & 31;     // g = 0: q = -1, the 12 bits before the row are zero
  uint32_t prev = (q >= 0) ? S[q] : 0u;
  #pragma unroll
  for (int w = 0; w < UnitQ<K>::NW; ++w) {
    const int i1 = w + q + 1; const uint32_t nxt = (i1 < WPS) ? S[i1] : 0u;
    u.w[w] = __funnelshift_r(prev, nxt, sh); prev = nxt;
  }
}
// `width` bits starting at the first bit of local state i (width 12 = the state; width 18 with K = 6 = states i and i + 1).
template <int K> __device__ __forceinline__ uint32_t unit_bits(const UnitQ<K>& u, int i, int width) {
  const int e = K * (i + 1), word = e >> 5, sh = e & 31; const uint32_t mask = (1u << width) - 1u;
  if (sh + width <= 32) return (u.w[word] >> sh) & mask;
  return __funnelshift_r(u.w[word], u.w[word + 1], sh) & mask;
}

// Shared memory of one block: [value table 16 KiB] [4 code stages] [input rows].
template <int K, int MT, int ST, int LK> __host__ __device__ constexpr int lut_bytes() { return (LK == 0 && ST >= 2) ? 4096 * 2 * REP : 0; }
template <int K> __host__ __device__ constexpr int stage_bytes() { return NT * 8 * 16 * K; }
template <int K, int MT, int ST, int LK> __host__ __device__ constexpr int smem_bytes() { return lut_bytes<K, MT, ST, LK>() + WPB * stage_bytes<K>() + (ST >= 4 ? MT * 16 * LDA : 0); }

// K  = code bits per trellis step (2 weights): 6 or 8.      MT = 16-row input groups per block (1 or 2).
// ST = last stage: 0 A read codes, 1 B states, 2 C lookup, 3 D int8 B fragment, 4 E mma (int32 sums), 5 F int64 accumulation, 6 G float.
// LK = lookup: 0 table in shared memory (engine), 1 none (state bits used as weights), 2 table in global memory (uint32[4096]),
//      3 pair table in global memory (uint32[2^18], index = two consecutive states, 4 weights per lookup; K = 6 only).
template <int K, int MT, int ST, int LK>
__global__ void __launch_bounds__(32 * WPB) k_gemm(Args P) {
  static_assert(LK != 3 || K == 6, "the pair table needs K = 6");
  extern __shared__ __align__(16) unsigned char smem[];
  constexpr int RB = 16 * K, SB = stage_bytes<K>();
  uint16_t* s_lut = reinterpret_cast<uint16_t*>(smem);
  unsigned char* my_ring = smem + lut_bytes<K, MT, ST, LK>() + (threadIdx.x >> 5) * SB;
  int8_t* sA = reinterpret_cast<int8_t*>(smem + lut_bytes<K, MT, ST, LK>() + WPB * SB);
  const int tid = threadIdx.x, warp = tid >> 5, lane = tid & 31, r = lane >> 2, g = lane & 3;
  const int N = P.N, Kc = P.Kc, M = P.M, KS = Kc / 256, G = Kc / 512;
  const int tile = blockIdx.x * WPB + warp, m0 = blockIdx.y * 16 * MT;
  const bool active = tile < N / 16;
  if constexpr (LK == 0 && ST >= 2) {
    for (int i = tid; i < 512; i += 32 * WPB) {                 // 8 entries per load, each written twice
      const uint4 v = __ldg(reinterpret_cast<const uint4*>(P.lut) + i);
      auto d = [](uint32_t e) { return e | (e << 16); };
      reinterpret_cast<uint4*>(smem)[2 * i]     = make_uint4(d(v.x & 0xFFFFu), d(v.x >> 16), d(v.y & 0xFFFFu), d(v.y >> 16));
      reinterpret_cast<uint4*>(smem)[2 * i + 1] = make_uint4(d(v.z & 0xFFFFu), d(v.z >> 16), d(v.w & 0xFFFFu), d(v.w >> 16));
    }
  }
  if constexpr (ST >= 4) for (int i = tid; i < MT * LDA; i += 32 * WPB) reinterpret_cast<uint4*>(sA)[i] = make_uint4(0u, 0u, 0u, 0u);   // rows >= M stay zero
  __syncthreads();
  const uint4* tbase = P.frag + (size_t)(active ? tile : 0) * KS * (NT * K * 8);
  const int rep = g & (REP - 1);
  uint32_t chk = 0;
  int32_t acc[NT][MT][4], s32[NT][MT][4]; long long acc64[NT][MT][4];
  #pragma unroll
  for (int j = 0; j < NT; ++j)
    #pragma unroll
    for (int mt = 0; mt < MT; ++mt)
      #pragma unroll
      for (int q = 0; q < 4; ++q) { acc[j][mt][q] = 0; s32[j][mt][q] = 0; acc64[j][mt][q] = 0; }

  for (int ks = 0; ks < KS; ++ks) {
    // input rows of this half -> shared memory (the whole block), 16 bytes per load
    if constexpr (ST >= 4) for (int i = tid; i < MT * 16 * 16; i += 32 * WPB) { const int tok = i >> 4, c = i & 15, gt = m0 + tok;
      if (gt < M) *reinterpret_cast<uint4*>(sA + tok * LDA + c * 16) = __ldg(reinterpret_cast<const uint4*>(P.qx + (size_t)gt * Kc + ks * 256 + c * 16)); }
    // codes of this half and of the 16 rows of the warp -> the warp's stage: NT * 8 * K loads of 16 bytes
    if (active) for (int i = lane; i < NT * 8 * K; i += 32) { const int j = i / (8 * K), rem = i % (8 * K), rr = rem / K, c = rem % K;
      const uint4 v = __ldg(tbase + ((size_t)(ks * NT + j) * K + c) * 8 + rr);
      if constexpr (ST == 0) chk ^= v.x ^ v.y ^ v.z ^ v.w;
      *reinterpret_cast<uint4*>(my_ring + (j * 8 + rr) * RB + c * 16) = v; }
    if constexpr (ST >= 4) __syncthreads(); else __syncwarp();

    if constexpr (ST >= 1) if (active) {
      if constexpr (ST >= 4) if ((ks & 1) == 0) {
        #pragma unroll
        for (int j = 0; j < NT; ++j)
          #pragma unroll
          for (int mt = 0; mt < MT; ++mt) acc[j][mt][0] = acc[j][mt][1] = acc[j][mt][2] = acc[j][mt][3] = 0;
      }
      UnitQ<K> u[NT];
      #pragma unroll
      for (int j = 0; j < NT; ++j) unit_load<K>(u[j], my_ring + (j * 8 + r) * RB, g);
      #pragma unroll
      for (int m = 0; m < 8; ++m) {
        uint2 bf[NT];
        #pragma unroll
        for (int j = 0; j < NT; ++j) {
          bf[j] = make_uint2(0u, 0u);
          if constexpr (LK == 3) bf[j] = make_uint2(P.glut18[unit_bits<K>(u[j], 4 * m, 18)], P.glut18[unit_bits<K>(u[j], 4 * m + 2, 18)]);
          else {
            const uint32_t s0 = unit_bits<K>(u[j], 4 * m, L_BITS), s1 = unit_bits<K>(u[j], 4 * m + 1, L_BITS);
            const uint32_t s2 = unit_bits<K>(u[j], 4 * m + 2, L_BITS), s3 = unit_bits<K>(u[j], 4 * m + 3, L_BITS);
            if constexpr (ST == 1) chk += s0 + s1 + s2 + s3;
            else {
              uint32_t a0, a1, b0, b1;
              if constexpr (LK == 0) { a0 = s_lut[s0 * REP + rep]; a1 = s_lut[s1 * REP + rep]; b0 = s_lut[s2 * REP + rep]; b1 = s_lut[s3 * REP + rep]; }
              else if constexpr (LK == 1) { a0 = s0; a1 = s1; b0 = s2; b1 = s3; }
              else { a0 = P.glut[s0] & 0xFFFFu; a1 = P.glut[s1] & 0xFFFFu; b0 = P.glut[s2] & 0xFFFFu; b1 = P.glut[s3] & 0xFFFFu; }
              if constexpr (ST == 2) chk += a0 + a1 + b0 + b1;
              else bf[j] = make_uint2(a0 | (a1 << 16), b0 | (b1 << 16));
            }
          }
          if constexpr (ST == 3) chk ^= bf[j].x ^ bf[j].y;
        }
        // A registers: {row r, k 4g..4g+3}, {row r+8, same k}, {row r, k 16+4g..}, {row r+8, same k}; k 4g+e = column 64g+8m+e, k 16+4g+e = column 64g+8m+4+e
        if constexpr (ST >= 4) {
          #pragma unroll
          for (int mt = 0; mt < MT; ++mt) {
            const int8_t* A0 = sA + (mt * 16 + r) * LDA + 64 * g + 8 * m;
            const uint2 lo = *reinterpret_cast<const uint2*>(A0), hi = *reinterpret_cast<const uint2*>(A0 + 8 * LDA);
            const uint32_t a[4] = {lo.x, hi.x, lo.y, hi.y};
            #pragma unroll
            for (int j = 0; j < NT; ++j) mma_s8_16832(acc[j][mt], a, bf[j]);
          }
        }
      }
      // end of a 512-column group: D registers = {row r, col 2g}, {row r, col 2g+1}, {row r+8, col 2g}, {row r+8, col 2g+1}; row = input row, col = output row
      if constexpr (ST >= 4) if (ks & 1) { const int grp = ks >> 1;
        #pragma unroll
        for (int j = 0; j < NT; ++j)
          #pragma unroll
          for (int mt = 0; mt < MT; ++mt)
            #pragma unroll
            for (int q = 0; q < 4; ++q) {
              if constexpr (ST == 4) s32[j][mt][q] += acc[j][mt][q];
              else { const int tok = m0 + mt * 16 + ((q < 2) ? r : r + 8);
                if (tok < M) acc64[j][mt][q] += (long long)P.Aw[(size_t)tok * G + grp] * (long long)acc[j][mt][q]; }
            }
      }
    }
    if constexpr (ST >= 4) __syncthreads();
  }
  if (!active) return;
  if constexpr (ST < 4) static_cast<uint32_t*>(P.out)[tile * 32 + lane] = chk;
  else {
    #pragma unroll
    for (int j = 0; j < NT; ++j)
      #pragma unroll
      for (int mt = 0; mt < MT; ++mt)
        #pragma unroll
        for (int q = 0; q < 4; ++q) {
          const int tok = m0 + mt * 16 + ((q < 2) ? r : r + 8), n = tile * 16 + j * 8 + 2 * g + (q & 1);
          if (tok < M) { const size_t o = (size_t)tok * N + n;
            if constexpr (ST == 4) static_cast<int32_t*>(P.out)[o] = s32[j][mt][q];
            else if constexpr (ST == 5) static_cast<long long*>(P.out)[o] = acc64[j][mt][q];
            else static_cast<float*>(P.out)[o] = __fmul_rn(__fmul_rn((float)acc64[j][mt][q], P.us[tok]), P.tau[n]); }
        }
  }
}

struct Grid { int bx, by, warps, smem; };
template <int K, int MT, int ST, int LK> Grid grid_of(const Args& P) {
  const int bx = (P.N / 16 + WPB - 1) / WPB, by = (P.M + 16 * MT - 1) / (16 * MT);
  return {bx, by, P.N / 16 * by, smem_bytes<K, MT, ST, LK>()};
}
template <int K, int MT, int ST, int LK> void launch(const Args& P) {
  const Grid gr = grid_of<K, MT, ST, LK>(P);
  k_gemm<K, MT, ST, LK><<<dim3(gr.bx, gr.by), 32 * WPB, gr.smem>>>(P);
}

// Host reference in natural column order for the output rows n0 .. n0 + nrows - 1 and all M input rows:
// ref[m * nrows + i] = sum_g Aw[m][g] * dot_g (Aw == nullptr: all 1).
inline std::vector<int64_t> host_ref(int K, const std::vector<uint32_t>& frag, const std::vector<uint16_t>& lut, const std::vector<int8_t>& qx,
                                     const int32_t* Aw, int N, int Kc, int M, int n0, int nrows) {
  const int G = Kc / 512, KS = Kc / 256; std::vector<int64_t> ref((size_t)M * nrows, 0); (void)N;
  for (int i = 0; i < nrows; ++i) { const int n = n0 + i, tile = n / 16, j = (n / 8) % NT, r = n % 8;
    for (int g = 0; g < G; ++g) { std::vector<int64_t> dot(M, 0);
      for (int h = 0; h < 2; ++h) { const int ks = 2 * g + h; uint32_t w[4 * 8 + 1] = {0};
        for (int c = 0; c < K; ++c) { const size_t u4 = ((((size_t)tile * KS + ks) * NT + j) * K + c) * 8 + r;
          for (int k = 0; k < 4; ++k) w[c * 4 + k] = frag[u4 * 4 + k]; }
        for (int J = 0; J < 128; ++J) { const int a = K * (J + 1) - L_BITS; uint32_t st;
          if (a < 0) st = (w[0] << (-a)) & 0xFFFu;
          else { const int wi = a >> 5, sh = a & 31; const uint64_t v = w[wi] | ((uint64_t)w[wi + 1] << 32); st = (uint32_t)(v >> sh) & 0xFFFu; }
          for (int e = 0; e < 2; ++e) { const int wv = (int8_t)(lut[st] >> (8 * e)); const size_t col = (size_t)ks * 256 + 2 * J + e;
            for (int m = 0; m < M; ++m) dot[m] += wv * (int)qx[(size_t)m * Kc + col]; } } }
      for (int m = 0; m < M; ++m) ref[(size_t)m * nrows + i] += (int64_t)(int32_t)dot[m] * (Aw ? Aw[(size_t)m * G + g] : 1); } }
  return ref;
}
}  // namespace gk
