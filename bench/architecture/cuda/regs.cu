// SPDX-License-Identifier: Apache-2.0
// Registers, threads and occupancy on CUDA (mirror of b60/regs.cpp).
//  live   : NR live 32-bit values per thread, all read and written in every loop trip -> where do spills start (255 registers per thread)
//           and how does the register count limit the number of resident threads?
//  group  : the same arithmetic kernel (16 live values) with different block sizes
//  threads: ... with different numbers of blocks (how many run at once), for blocks of 32 and of 256 threads
//  smem   : ... with dynamic shared memory per block (does shared memory limit the number of resident blocks?)
//  launch : cost of a kernel launch (empty kernel), synchronous and queued
// The registers per thread chosen by the compiler are read from the "ptxas -v" log written by scripts/build_cuda.sh:
// usage: regs [ptxas log]     (default: <path of this binary>.ptxas.log; column ptxas_registers = -1 when not available)
//        env ARCH_ONLY=live,group,threads,smem,launch
#include "common.cuh"
#include <map>

template <int NR>
__global__ void k_live(uint32_t* out, int iters) {
  const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
  uint32_t r[NR];
  #pragma unroll
  for (int k = 0; k < NR; ++k) r[k] = gid * 7u + (uint32_t)k;
  for (int it = 0; it < iters; ++it) {
    #pragma unroll
    for (int k = 0; k < NR; ++k) r[k] = (r[k] + r[(k + 1) % NR]) ^ 0x9E3779B1u;
  }
  uint32_t s = 0;
  #pragma unroll
  for (int k = 0; k < NR; ++k) s += r[k];
  out[gid] = s;
}
static uint32_t live_ref(int NR, uint32_t gid, int iters) {
  std::vector<uint32_t> r(NR); for (int k = 0; k < NR; ++k) r[k] = gid * 7u + (uint32_t)k;
  for (int it = 0; it < iters; ++it) for (int k = 0; k < NR; ++k) r[k] = (r[k] + r[(k + 1) % NR]) ^ 0x9E3779B1u;
  uint32_t s = 0; for (int k = 0; k < NR; ++k) s += r[k]; return s;
}
// 16 live values; `words` dwords of dynamic shared memory per block (0 = none; at least blockDim.x otherwise)
__global__ void k_work(uint32_t* out, int iters, uint32_t words) {
  constexpr int NR = 16;
  extern __shared__ uint32_t sm[];
  const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
  if (words) { for (uint32_t j = threadIdx.x; j < words; j += blockDim.x) sm[j] = j; __syncthreads(); }
  uint32_t r[NR];
  #pragma unroll
  for (int k = 0; k < NR; ++k) r[k] = gid * 7u + (uint32_t)k;
  for (int it = 0; it < iters; ++it) {
    #pragma unroll
    for (int k = 0; k < NR; ++k) r[k] = (r[k] + r[(k + 1) % NR]) ^ 0x9E3779B1u;
  }
  uint32_t s = 0;
  #pragma unroll
  for (int k = 0; k < NR; ++k) s += r[k];
  if (words) s += sm[threadIdx.x];
  out[gid] = s;
}
__global__ void k_empty(uint32_t* out) { out[0] = 1; }

// "... Compiling entry function '_Z6k_liveILi64EEvPji' ..." followed by "... Used 70 registers ..."
static std::map<int, int> ptxas_regs(const std::string& path) {
  std::map<int, int> m; FILE* f = std::fopen(path.c_str(), "r"); if (!f) return m;
  char line[2048]; int cur = -1;
  while (std::fgets(line, sizeof line, f)) {
    if (const char* p = std::strstr(line, "k_liveILi")) cur = std::atoi(p + 9);
    else if (const char* u = std::strstr(line, "Used ")) { int n; if (cur >= 0 && std::sscanf(u, "Used %d registers", &n) == 1) m[cur] = n; cur = -1; }
  }
  std::fclose(f); return m;
}

static uint32_t* g_out = nullptr; static std::map<int, int> g_regs;
static void row(const char* sec, const char* p, int v, int T, int BS, int iters, int nr, double s, int regs = -1, const char* chk = "-") {
  if (s <= 0) return;
  std::printf("%s,%s,%d,%d,%d,%d,%.4f,%.3f,%.3f,%d,%d,%s\n", sec, p, v, T, BS, iters, s * 1e3, s * 1e9 / ((double)iters * nr), (double)T * iters * nr / s / 1e9, T / BS, regs, chk);
  std::fflush(stdout);
}
template <int NR> static void live() {
  for (int T : {2560, 81920}) { const int iters = 400000 / NR, BS = 128;      // = 160 and 5120 B60 threads of 16 lanes
    const double s = ab::best_time([&] { k_live<NR><<<T / BS, BS>>>(g_out, iters); }, true, 0.03, 4);
    const std::vector<uint32_t> o = ab::download<uint32_t>(g_out, T); const char* chk = "exact";
    for (uint32_t gid : {0u, 37u, (uint32_t)T - 1u}) if (o[gid] != live_ref(NR, gid, iters)) chk = "MISMATCH";
    row("live", "live_u32_values", NR, T, BS, iters, NR, s, g_regs.count(NR) ? g_regs[NR] : -1, chk); }
}
static double work(int blocks, int BS, int iters, int smem_bytes) {
  if (!ab::ck(cudaFuncSetAttribute(k_work, cudaFuncAttributeMaxDynamicSharedMemorySize, smem_bytes), "shared memory size")) return -1;
  return ab::best_time([&] { k_work<<<blocks, BS, smem_bytes>>>(g_out, iters, (uint32_t)smem_bytes / 4); }, true, 0.03, 4);
}

int main(int argc, char** argv) {
  ab::banner("regs");
  g_regs = ptxas_regs(argc > 1 ? std::string(argv[1]) : std::string(argv[0]) + ".ptxas.log");
  const int sm = ab::prop().multiProcessorCount, TMAX = std::max(1 << 20, sm * 128 * 256);
  g_out = ab::dalloc<uint32_t>(TMAX);
  std::printf("section,param,value,threads,group_size,iters,ms,ns_per_update_in_thread,G_register_updates_per_s,blocks,ptxas_registers,check\n");
  std::printf("# CUDA: threads = CUDA threads, group_size = block size\n");
  if (ab::want("live")) {
    live<8>(); live<16>(); live<32>(); live<48>(); live<64>(); live<80>(); live<96>(); live<104>(); live<112>(); live<116>(); live<120>(); live<124>(); live<128>();
    live<144>(); live<160>(); live<192>(); live<224>(); live<240>(); live<248>(); live<256>(); live<288>(); live<320>();
  }
  const int IT = 20000;
  ab::soft() = true;          // an impossible configuration prints a "# error" line and is skipped
  if (ab::want("group")) for (int BS : {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024}) row("group", "block_size", BS, 65536, BS, IT, 16, work(65536 / BS, BS, IT, 0));
  if (ab::want("threads")) for (int BS : {32, 256}) {
    std::vector<int> nb = {1, 2, 4, 8, 16, 32};
    for (int k : {1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128}) if (sm * k > nb.back()) nb.push_back(sm * k);      // per SM: 32 blocks or 1536 threads are resident at most
    for (int b : nb) row("threads", "blocks", b, b * BS, BS, IT, 16, work(b, BS, IT, 0));
  }
  if (ab::want("smem")) for (int kib : {0, 4, 16, 32, 64, 99}) for (int BS : {32, 256}) for (int k : {8, 16, 32, 48, 96, 192}) {
    const int T = sm * 32 * k;          // 48 = every SM full of threads (blocks of 32: limited to 32 blocks per SM)
    row("smem", "smem_bytes_per_block", kib * 1024, T, BS, IT, 16, work(T / BS, BS, IT, kib * 1024));
  }
  ab::soft() = false;
  if (ab::want("launch")) {
    std::printf("section,kernel,mode,us_per_launch\n");
    auto l = [&] { k_empty<<<1, 1>>>(g_out); };
    std::printf("launch,empty_cuda,launch+synchronize,%.2f\n", ab::best_time(l, true, 0.05, 5) * 1e6);
    std::printf("launch,empty_cuda,queued (one synchronize per batch),%.2f\n", ab::best_time(l, false, 0.05, 5) * 1e6);
    { double best = 1e30; for (int r = 0; r < 20; ++r) { const double t0 = ab::now(); cudaStreamSynchronize(0); best = std::min(best, ab::now() - t0); }
      std::printf("launch,none,cudaStreamSynchronize on an idle stream,%.2f\n", best * 1e6); }
    { std::vector<uint32_t> h(16); double best = 1e30;
      for (int r = 0; r < 50; ++r) { const double t0 = ab::now(); cudaMemcpy(h.data(), g_out, 64, cudaMemcpyDeviceToHost); best = std::min(best, ab::now() - t0); }
      std::printf("launch,none,64-byte device-to-host cudaMemcpy,%.2f\n", best * 1e6); }
  }
  cudaFree(g_out);
  return 0;
}
