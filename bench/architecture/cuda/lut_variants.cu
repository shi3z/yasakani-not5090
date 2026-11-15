// SPDX-License-Identifier: Apache-2.0
// Value-table variants for the compressed GEMM on CUDA (mirror of b60/lut_variants.cpp).
//  micro : lookups per second of one table-lookup primitive in a tight loop.  One CUDA thread = one lookup per step (a warp = 32 lookups).
//          none | smem_u32 (uint32[4096] = 16 KiB in shared memory) | smem_u16 (uint16[4096] = 8 KiB in shared memory)
//          | global_u32 (16 KiB and 1 MiB in global memory)
//          index patterns: random (every thread its own LCG), blocked (the 32 lanes of a warp read 32 consecutive entries), const (one entry per warp)
//  kernel: the whole GEMM kernel (3-bit codes, 8 input rows, int32 sums = stage E) with the lookup replaced:
//          none (state bits used as weights) | shared (engine) | global 16 KiB | global 1 MiB pair table (two consecutive states -> 4 weights per lookup)
//          each table variant also with an all-zero code stream (all lookups hit one entry).
// usage: lut_variants      env ARCH_ONLY=micro,kernel
#include "gemm_kernel.cuh"
constexpr uint32_t LA = 1664525u, LC = 1013904223u;

// KIND 0 none, 1 shared uint32[4096], 2 shared uint16[4096], 3 global uint32 (mask given).  PAT 0 random, 1 blocked, 2 const.
template <int KIND, int PAT>
__global__ void k_micro(const uint32_t* tab, const uint16_t* tab16, uint32_t* out, int iters, uint32_t mask) {
  extern __shared__ __align__(16) unsigned char smem[];
  const uint32_t tid = threadIdx.x, gid = blockIdx.x * blockDim.x + tid, lane = tid & 31u, gwarp = gid >> 5;
  uint32_t* s32 = reinterpret_cast<uint32_t*>(smem); uint16_t* s16 = reinterpret_cast<uint16_t*>(smem);
  if constexpr (KIND == 1) { for (uint32_t i = tid; i < 4096; i += blockDim.x) s32[i] = tab[i]; __syncthreads(); }
  if constexpr (KIND == 2) { for (uint32_t i = tid; i < 4096; i += blockDim.x) s16[i] = tab16[i]; __syncthreads(); }
  uint32_t idx = 0x9E3779B1u * (gid + 1u), acc = 0, sc = gwarp * 0x9E3779B1u + 1u;
  for (int it = 0; it < iters; ++it) {
    uint32_t ix;
    if constexpr (PAT == 0) { idx = idx * LA + LC; ix = idx & mask; }
    else if constexpr (PAT == 1) { sc = sc * LA + LC; ix = lane + ((sc >> 8) & mask & ~31u); }
    else { sc = sc * LA + LC; ix = (sc >> 8) & mask; }
    if constexpr (KIND == 0) acc += ix << 2;
    else if constexpr (KIND == 1) acc += s32[ix];
    else if constexpr (KIND == 2) acc += s16[ix];
    else acc += tab[ix];
  }
  out[gid] = acc;
}
// Host model of one thread of k_micro.
static uint32_t micro_ref(int kind, int pat, const std::vector<uint32_t>& tab, uint32_t gid, int iters, uint32_t mask) {
  const uint32_t lane = gid & 31u, gwarp = gid >> 5; uint32_t idx = 0x9E3779B1u * (gid + 1u), acc = 0, sc = gwarp * 0x9E3779B1u + 1u;
  for (int it = 0; it < iters; ++it) { uint32_t ix;
    if (pat == 0) { idx = idx * LA + LC; ix = idx & mask; } else if (pat == 1) { sc = sc * LA + LC; ix = lane + ((sc >> 8) & mask & ~31u); } else { sc = sc * LA + LC; ix = (sc >> 8) & mask; }
    acc += kind == 0 ? ix << 2 : kind == 2 ? (uint32_t)(uint16_t)tab[ix] : tab[ix]; }
  return acc;
}

struct Micro { const uint32_t* tab; const uint16_t* tab16; uint32_t* out; const std::vector<uint32_t>* htab; };
template <int KIND, int PAT> static void micro(const Micro& c, const char* prim, int T, int iters, uint32_t mask, int table_bytes) {
  static const char* pn[] = {"random", "blocked", "const"};
  const int BS = 256, smem = KIND == 1 ? 16384 : KIND == 2 ? 8192 : 0;          // T is a multiple of 256
  auto launch = [&] { k_micro<KIND, PAT><<<T / BS, BS, smem>>>(c.tab, c.tab16, c.out, iters, mask); };
  launch(); ab::launched(); ab::sync();
  const std::vector<uint32_t> o = ab::download<uint32_t>(c.out, T); const char* chk = "exact";
  for (uint32_t gid : {0u, 37u, (uint32_t)T - 1u}) if (o[gid] != micro_ref(KIND, PAT, *c.htab, gid, iters, mask)) chk = "MISMATCH";
  const double s = ab::best_time(launch, true);
  std::printf("micro,%s,%s,%d,%d,%d,%.4f,%.2f,%.2f,%d,%s\n", prim, pn[PAT], table_bytes, T, iters, s * 1e3, (double)T * iters / s / 1e9, s * 1e9 / iters, BS, chk);
  std::fflush(stdout);
}

struct Kern { gk::Args P; const uint4* st; const uint4* st0; int32_t* o; int32_t* o2; double gw; };
template <int LK> static void kern(const Kern& c, const char* name, bool zero, const char* chk) {
  gk::Args P = c.P; P.frag = zero ? c.st0 : c.st;
  auto launch = [&] { gk::launch<6, 1, 4, LK>(P); };
  const double tq = ab::best_time(launch, false), ts = ab::best_time(launch, true);
  std::printf("kernel,%s,%s,%.4f,%.4f,%.1f,%s,%d\n", name, zero ? "all zero (one entry)" : "random", tq * 1e3, ts * 1e3, c.gw / tq, chk, gk::smem_bytes<6, 1, 4, LK>());
  std::fflush(stdout);
}
template <int LK> static std::vector<int32_t> sums(const Kern& c) {
  gk::Args P = c.P; P.frag = c.st; P.out = c.o2;
  gk::launch<6, 1, 4, LK>(P); ab::launched(); ab::sync();
  return ab::download<int32_t>(c.o2, (size_t)P.M * P.N);
}

int main() {
  ab::banner("lut_variants");
  uint64_t seed = 7;
  std::vector<uint32_t> hl(1 << 18); for (auto& v : hl) v = ab::rng(seed) & 0xffffu;
  std::vector<uint32_t> h18(1 << 18); for (uint32_t i = 0; i < (1u << 18); ++i) h18[i] = (hl[i & 0xfff] & 0xffffu) | (hl[i >> 6] << 16);
  std::vector<uint16_t> h16(4096); for (int i = 0; i < 4096; ++i) h16[i] = (uint16_t)hl[i];
  uint32_t* tab = ab::upload(hl); uint32_t* t18 = ab::upload(h18); uint16_t* t16 = ab::upload(h16);
  if (ab::want("micro")) {
    std::printf("section,primitive,pattern,table_bytes,threads,iters,ms,G_lookups_per_s,ns_per_gather16_in_thread,block_size,check\n");
    std::printf("# CUDA: threads = CUDA threads (one lookup per step each); ns_per_gather16_in_thread = ns per step of one thread\n");
    const int TMAX = 1 << 20; uint32_t* out = ab::dalloc<uint32_t>(TMAX);
    const Micro c{tab, t16, out, &hl};
    for (int T : {10240, 65536, TMAX}) { const int it = T > 100000 ? 16384 : 65536; const uint32_t m12 = 4095, m18 = (1u << 18) - 1;
      micro<0, 0>(c, "none", T, it, m12, 16384); micro<0, 1>(c, "none", T, it, m12, 16384); micro<0, 2>(c, "none", T, it, m12, 16384);
      micro<1, 0>(c, "smem_u32", T, it, m12, 16384); micro<1, 1>(c, "smem_u32", T, it, m12, 16384); micro<1, 2>(c, "smem_u32", T, it, m12, 16384);
      micro<2, 0>(c, "smem_u16", T, it, m12, 8192); micro<2, 1>(c, "smem_u16", T, it, m12, 8192); micro<2, 2>(c, "smem_u16", T, it, m12, 8192);
      micro<3, 0>(c, "global_u32", T, it, m12, 16384); micro<3, 1>(c, "global_u32", T, it, m12, 16384); micro<3, 2>(c, "global_u32", T, it, m12, 16384);
      micro<3, 0>(c, "global_u32", T, it, m18, 1 << 20); micro<3, 1>(c, "global_u32", T, it, m18, 1 << 20);
    }
    cudaFree(out);
  }
  if (ab::want("kernel")) {
    const int N = 34816, Kc = 5120, K = 6, M = 8; const size_t nstream = (size_t)N * Kc * K / 64;
    std::vector<uint32_t> hs(nstream); for (auto& v : hs) v = ab::rng(seed);
    std::vector<int8_t> hq((size_t)Kc * M); for (auto& v : hq) v = (int8_t)(ab::rng(seed) >> 24);
    uint32_t* st = ab::upload(hs); uint32_t* st0 = ab::dalloc<uint32_t>(nstream); int8_t* qx = ab::upload(hq);
    ab::ck(cudaMemset(st0, 0, nstream * 4), "memset");
    int32_t* o = ab::dalloc<int32_t>((size_t)M * N); int32_t* o2 = ab::dalloc<int32_t>((size_t)M * N);
    Kern c; c.st = reinterpret_cast<const uint4*>(st); c.st0 = reinterpret_cast<const uint4*>(st0); c.o = o; c.o2 = o2; c.gw = (double)N * Kc / 1e9;
    c.P = gk::Args{c.st, t16, tab, t18, qx, nullptr, nullptr, nullptr, o, N, Kc, M};
    std::printf("section,lookup,code_stream,ms_queued,ms_sync,Gweights_per_s,check,smem_bytes_per_block\n");
    // shared-memory variant against the host (three tiles), the other tables against the shared-memory variant (all sums)
    const std::vector<int32_t> a = sums<0>(c), b = sums<2>(c), d = sums<3>(c);
    const char* c1 = "exact";
    for (int n0 : {0, (N / 2) & ~15, N - 16}) { const std::vector<int64_t> ref = gk::host_ref(K, hs, h16, hq, nullptr, N, Kc, M, n0, 16);
      for (int m = 0; m < M; ++m) for (int i = 0; i < 16; ++i) if ((int64_t)a[(size_t)m * N + n0 + i] != ref[(size_t)m * 16 + i]) c1 = "MISMATCH"; }
    const char* c2 = a == b ? "same sums as shared" : "DIFFERENT", *c3 = a == d ? "same sums as shared" : "DIFFERENT";
    kern<1>(c, "none (state bits as weights)", false, "-");
    kern<0>(c, "shared-memory table (engine)", false, c1);
    kern<0>(c, "shared-memory table (engine)", true, "-");
    kern<2>(c, "global table 16 KiB", false, c2);
    kern<2>(c, "global table 16 KiB", true, "-");
    kern<3>(c, "global 1 MiB pair table (4 weights/lookup)", false, c3);
    kern<3>(c, "global 1 MiB pair table (4 weights/lookup)", true, "-");
    cudaFree(st); cudaFree(st0); cudaFree(qx); cudaFree(o); cudaFree(o2);
  }
  cudaFree(tab); cudaFree(t18); cudaFree(t16);
  return 0;
}
