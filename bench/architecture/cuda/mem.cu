// SPDX-License-Identifier: Apache-2.0
// Memory hierarchy of a CUDA GPU seen from kernels: global memory and shared memory (mirror of b60/mem.cpp).
// One CUDA thread = one access per step (the B60 program does 16 lanes per thread), so "threads" here are B60 threads x 16.
//   seq_read / seq_write / copy : 16-byte (uint4) loads / stores per thread; the 32 lanes of a warp cover 512 contiguous bytes per step and
//                                 each warp walks its own contiguous range (wraps inside the working set)
//   strided                     : one 16-byte load per thread every S bytes (S a multiple of 64)
//   gather / scatter            : one random 4-byte access per thread and step (index from an in-register LCG)
//   chase                       : one dependent chain per thread, next index = LCG(value read)  -> latency per access
//   smem_*                      : the same on shared memory (blocks of 256 threads; the table is `working_set_bytes` per block)
// The first GiB of `src` holds src[i] = i, so chase walks one full-period permutation of the working set (no merging chains).
// Allocates 1 GiB + 512 MiB of device memory (the 4 MiB of per-thread results live at the start of the 512 MiB buffer).
// usage: mem      env ARCH_ONLY=ws,threads,stride,smem
#include "common.cuh"
constexpr uint32_t LA = 1664525u, LC = 1013904223u, SEED = 0x9E3779B1u;
#define GID const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x, lane = threadIdx.x & 31u, gw = gid >> 5; (void)lane; (void)gw

__global__ void k_iota(uint32_t* p, size_t n) { for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) p[i] = (uint32_t)i; }
__global__ void k_seq_read(const uint4* src, uint32_t* out, int iters, uint32_t mb) {       // mb = working set / 512 - 1
  GID; const uint32_t b = gw * (uint32_t)iters; uint32_t chk = 0;
  for (int it = 0; it < iters; ++it) { const uint4 v = src[(size_t)((b + it) & mb) * 32 + lane]; chk ^= v.x ^ v.y ^ v.z ^ v.w; }
  out[gid] = chk;
}
__global__ void k_seq_write(uint4* dst, int iters, uint32_t mb) {
  GID; const uint32_t b = gw * (uint32_t)iters; uint4 v = make_uint4(gid, gid + 1u, gid + 2u, gid + 3u);
  for (int it = 0; it < iters; ++it) { dst[(size_t)((b + it) & mb) * 32 + lane] = v; v.x += 3u; v.y += 3u; v.z += 3u; v.w += 3u; }
}
__global__ void k_copy(const uint4* src, uint4* dst, int iters, uint32_t mb) {
  GID; const uint32_t b = gw * (uint32_t)iters;
  for (int it = 0; it < iters; ++it) { const size_t o = (size_t)((b + it) & mb) * 32 + lane; dst[o] = src[o]; }
}
__global__ void k_strided(const uint4* src, uint32_t* out, int iters, uint32_t stride64, uint32_t m64) {   // units of 64 bytes
  GID; const uint32_t b = gid * (uint32_t)iters; uint32_t chk = 0;
  for (int it = 0; it < iters; ++it) { const uint4 v = src[(size_t)(((b + it) * stride64) & m64) * 4]; chk ^= v.x ^ v.y ^ v.z ^ v.w; }
  out[gid] = chk;
}
__global__ void k_gather(const uint32_t* src, uint32_t* out, int iters, uint32_t mask) {
  GID; uint32_t idx = SEED * (gid + 1u), acc = 0;
  for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += src[idx & mask]; }
  out[gid] = acc;
}
__global__ void k_nogather(uint32_t* out, int iters, uint32_t mask) {     // the index arithmetic alone
  GID; uint32_t idx = SEED * (gid + 1u), acc = 0;
  for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += (idx & mask) << 2; }
  out[gid] = acc;
}
__global__ void k_scatter(uint32_t* dst, int iters, uint32_t mask) {
  GID; uint32_t idx = SEED * (gid + 1u);
  for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; dst[idx & mask] = idx; }
}
__global__ void k_chase(const uint32_t* src, uint32_t* out, int iters, uint32_t mask) {
  GID; uint32_t idx = SEED * (gid + 1u);
  for (int it = 0; it < iters; ++it) { const uint32_t v = src[idx & mask]; idx = v * LA + LC; }
  out[gid] = idx;
}
// Shared memory, `bytes` per block (a power of two >= 4096), table[i] = i.
// MODE 0 sequential 16-byte loads (512 bytes per warp and step), 1 random 4-byte loads, 2 dependent loads, 3 baseline without shared-memory access.
template <int MODE>
__global__ void k_smem(uint32_t* out, int iters, uint32_t bytes) {
  extern __shared__ __align__(16) uint32_t sm[];
  GID;
  for (uint32_t i = threadIdx.x; i < bytes / 4; i += blockDim.x) sm[i] = i;
  __syncthreads();
  const uint32_t mask = bytes / 4 - 1, mb = bytes / 512 - 1;
  uint32_t idx = SEED * (gid + 1u), acc = 0;
  if constexpr (MODE == 0) { const uint32_t b = gw * 7u; const uint4* s4 = reinterpret_cast<const uint4*>(sm);
    for (int it = 0; it < iters; ++it) { const uint4 v = s4[((b + it) & mb) * 32 + lane]; acc ^= v.x ^ v.y ^ v.z ^ v.w; } }
  else if constexpr (MODE == 1) for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += sm[idx & mask]; }
  else if constexpr (MODE == 2) { for (int it = 0; it < iters; ++it) { const uint32_t v = sm[idx & mask]; idx = v * LA + LC; } acc = idx; }
  else for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += (idx & mask) << 2; }
  out[gid] = acc;
}

// Host models of one thread (the tables hold table[i] = i).
static uint32_t ref_seq(uint32_t gid, uint32_t b, int iters, uint32_t mb) { uint32_t chk = 0;
  for (int it = 0; it < iters; ++it) { const uint32_t w = (((b + it) & mb) * 32 + (gid & 31u)) * 4; chk ^= w ^ (w + 1) ^ (w + 2) ^ (w + 3); } return chk; }
static uint32_t ref_strided(uint32_t gid, int iters, uint32_t stride64, uint32_t m64) { const uint32_t b = gid * (uint32_t)iters; uint32_t chk = 0;
  for (int it = 0; it < iters; ++it) { const uint32_t w = (((b + it) * stride64) & m64) * 16; chk ^= w ^ (w + 1) ^ (w + 2) ^ (w + 3); } return chk; }
static uint32_t ref_gather(uint32_t gid, int iters, uint32_t mask, bool shifted) { uint32_t idx = SEED * (gid + 1u), acc = 0;
  for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += shifted ? (idx & mask) << 2 : idx & mask; } return acc; }
static uint32_t ref_chase(uint32_t gid, int iters, uint32_t mask) { uint32_t idx = SEED * (gid + 1u);
  for (int it = 0; it < iters; ++it) idx = (idx & mask) * LA + LC; return idx; }

static uint32_t* g_out = nullptr;
// Compares the results of threads 0, 37 % T and T - 1 of the last launch with the host model.
template <class F> static const char* verify(int T, F&& ref) {
  const std::vector<uint32_t> o = ab::download<uint32_t>(g_out, T);
  for (uint32_t gid : {0u, 37u % (uint32_t)T, (uint32_t)T - 1u}) if (o[gid] != ref(gid)) return "MISMATCH";
  return "exact";
}
static void row(const char* sec, const char* test, size_t ws, int T, int param, double s, double bytes, double acc, double ops_in_thread, const char* chk = "-") {
  if (s <= 0) return;
  std::printf("%s,%s,%zu,%d,%d,%.4f,%.1f,%.1f,%.2f,%s\n", sec, test, ws, T, param, s * 1e3, bytes / s / 1e9, acc / s / 1e6, s * 1e9 / ops_in_thread, chk); std::fflush(stdout);
}
struct Dim { int blocks, bt; };
static Dim dim_of(int T) { const int bt = std::min(T, 256); return {T / bt, bt}; }      // T <= 256 or a multiple of 256
#define LAUNCH(kernel, T, ...) [&] { const Dim d_ = dim_of(T); kernel<<<d_.blocks, d_.bt>>>(__VA_ARGS__); }

template <int MODE> static double smem_time(int T, int iters, int bytes) {
  if (!ab::ck(cudaFuncSetAttribute(k_smem<MODE>, cudaFuncAttributeMaxDynamicSharedMemorySize, bytes), "shared memory size")) return -1;
  return ab::best_time([&] { k_smem<MODE><<<T / 256, 256, bytes>>>(g_out, iters, (uint32_t)bytes); }, true);
}

int main() {
  ab::banner("mem");
  const size_t GB = 1ull << 30;
  uint32_t* src = ab::dalloc<uint32_t>(GB / 4); uint32_t* dst = ab::dalloc<uint32_t>(GB / 8); uint32_t* out = dst; g_out = out;
  const uint4* src4 = reinterpret_cast<const uint4*>(src); uint4* dst4 = reinterpret_cast<uint4*>(dst);
  k_iota<<<4096, 256>>>(src, GB / 4); ab::launched(); ab::ck(cudaMemset(dst, 0, GB / 2), "memset"); ab::sync();
  std::printf("section,test,working_set_bytes,threads,param,ms,GB_per_s,Maccess_per_s,ns_per_op_in_thread,check\n");
  std::printf("# CUDA: threads = CUDA threads; seq/copy: 16 bytes per thread and step (512 contiguous bytes per warp); gather/scatter/chase: one 4-byte access per thread and step\n");
  const size_t wss[] = {4096, 16384, 65536, 262144, 1u << 20, 4u << 20, 16u << 20, 64u << 20, 256u << 20, GB};
  if (ab::want("ws")) for (size_t ws : wss) {
    const int T = 65536, it = 1024, it1 = 20000; const uint32_t mb = (uint32_t)(ws / 512 - 1), mask = (uint32_t)(ws / 4 - 1);
    const double bytes = (double)T * it * 16, acc = (double)T * it;
    double s = ab::best_time(LAUNCH(k_seq_read, T, src4, out, it, mb), true);
    row("ws", "seq_read", ws, T, 0, s, bytes, 0, it, verify(T, [&](uint32_t g) { return ref_seq(g, (g >> 5) * (uint32_t)it, it, mb); }));
    s = ab::best_time(LAUNCH(k_gather, T, src, out, it, mask), true);
    row("ws", "gather", ws, T, 0, s, acc * 4, acc, it, verify(T, [&](uint32_t g) { return ref_gather(g, it, mask, false); }));
    s = ab::best_time(LAUNCH(k_chase, T, src, out, it, mask), true);
    row("ws", "chase_65536thr", ws, T, 0, s, acc * 4, acc, it, verify(T, [&](uint32_t g) { return ref_chase(g, it, mask); }));
    s = ab::best_time(LAUNCH(k_chase, 1, src, out, it1, mask), true);
    row("ws", "chase_1thr", ws, 1, 0, s, it1 * 4.0, it1, it1, verify(1, [&](uint32_t g) { return ref_chase(g, it1, mask); }));
    if (ws <= GB / 2) {       // these overwrite the result area at the start of dst, so they come after the checks
      row("ws", "seq_write", ws, T, 0, ab::best_time(LAUNCH(k_seq_write, T, dst4, it, mb), true), bytes, 0, it);
      row("ws", "copy", ws, T, 0, ab::best_time(LAUNCH(k_copy, T, src4, dst4, it, mb), true), bytes, 0, it);
      row("ws", "scatter", ws, T, 0, ab::best_time(LAUNCH(k_scatter, T, dst, it, mask), true), acc * 4, acc, it);
    }
  }
  if (ab::want("ws")) { const int T = 65536, it = 1024; const double s = ab::best_time(LAUNCH(k_nogather, T, out, it, 0xffffu), true);
    row("ws", "index_arith_only", 0, T, 0, s, 0, (double)T * it, it, verify(T, [&](uint32_t g) { return ref_gather(g, it, 0xffffu, true); })); }
  if (ab::want("threads")) for (int T : {32, 256, 1024, 2560, 5120, 10240, 20480, 40960, 81920, 262144, 1048576}) {
    const int it = T <= 1024 ? 16384 : T <= 81920 ? 2048 : 512; const double acc = (double)T * it;
    const uint32_t mbG = (uint32_t)(GB / 512 - 1), maskG = (uint32_t)(GB / 4 - 1);
    row("threads", "seq_read", GB, T, 0, ab::best_time(LAUNCH(k_seq_read, T, src4, out, it, mbG), true), acc * 16, 0, it);
    row("threads", "seq_read", 65536, T, 0, ab::best_time(LAUNCH(k_seq_read, T, src4, out, it, 127u), true), acc * 16, 0, it);
    row("threads", "gather", GB, T, 0, ab::best_time(LAUNCH(k_gather, T, src, out, it, maskG), true), acc * 4, acc, it);
    row("threads", "gather", 16384, T, 0, ab::best_time(LAUNCH(k_gather, T, src, out, it, 4095u), true), acc * 4, acc, it);
    row("threads", "chase", GB, T, 0, ab::best_time(LAUNCH(k_chase, T, src, out, it, maskG), true), acc * 4, acc, it);
  }
  if (ab::want("stride")) for (size_t ws : {(size_t)(256u << 20), GB}) for (int st : {1, 2, 4, 16, 64, 1024}) {
    const int T = 65536, it = 2048; const uint32_t m64 = (uint32_t)(ws / 64 - 1);
    const double s = ab::best_time(LAUNCH(k_strided, T, src4, out, it, (uint32_t)st, m64), true);
    row("stride", "strided_read_16B", ws, T, st * 64, s, (double)T * it * 16, (double)T * it, it, verify(T, [&](uint32_t g) { return ref_strided(g, it, (uint32_t)st, m64); }));
  }
  if (ab::want("smem")) {
    ab::soft() = true;        // a size above the device limit prints a "# error" line and is skipped
    for (int SZ : {4096, 16384, 32768, 65536}) for (int T : {512, 10240, 20480, 65536}) { const int it = 16384; const double n = (double)T * it;
      const uint32_t mb = (uint32_t)SZ / 512 - 1, mask = (uint32_t)SZ / 4 - 1;
      double s = smem_time<0>(T, it, SZ);
      if (s > 0) row("smem", "smem_seq_read", SZ, T, 256, s, n * 16, 0, it, verify(T, [&](uint32_t g) { return ref_seq(g, (g >> 5) * 7u, it, mb); }));
      s = smem_time<1>(T, it, SZ);
      if (s > 0) row("smem", "smem_gather", SZ, T, 256, s, n * 4, n, it, verify(T, [&](uint32_t g) { return ref_gather(g, it, mask, false); }));
      s = smem_time<2>(T, it, SZ);
      if (s > 0) row("smem", "smem_chase", SZ, T, 256, s, n * 4, n, it, verify(T, [&](uint32_t g) { return ref_chase(g, it, mask); }));
      s = smem_time<3>(T, it, SZ);
      if (s > 0) row("smem", "smem_none(index arith)", SZ, T, 256, s, 0, n, it, verify(T, [&](uint32_t g) { return ref_gather(g, it, mask, true); }));
    }
    ab::soft() = false;
  }
  cudaFree(src); cudaFree(dst);
  return 0;
}
