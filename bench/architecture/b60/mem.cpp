// SPDX-License-Identifier: Apache-2.0
// Memory hierarchy of the B60 seen from ESIMD kernels: global (USM) memory and shared local memory (SLM).
//   seq_read / seq_write / copy : 256-byte block loads / stores, each thread walks its own contiguous range (wraps inside the working set)
//   strided                     : one 64-byte load every S bytes
//   gather / scatter            : 16 independent random 4-byte accesses per call (indices from an in-register LCG)
//   chase                       : 16 dependent chains per thread, next index = LCG(value read)  -> latency per access
//   slm_*                       : the same on SLM (work-groups of 32 threads; the table is SLM_BYTES per work-group)
// The first GiB of `src` holds src[i] = i, so chase walks one full-period permutation of the working set (no merging chains).
// Allocates 1 GiB + 512 MiB of device memory.
// usage: mem      env ARCH_ONLY=ws,threads,stride,slm,data   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;
constexpr uint32_t LA = 1664525u, LC = 1013904223u;

static event k_seq_read(queue& q, const uint32_t* src, uint32_t* out, int T, int iters, uint32_t mb) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const uint32_t b = (uint32_t)i[0] * (uint32_t)iters; simd<uint32_t, 64> chk = 0;
    for (int it = 0; it < iters; ++it) chk ^= block_load<uint32_t, 64>(src + (size_t)((b + it) & mb) * 64);
    block_store<uint32_t, 16>(out + i[0] * 16, chk.select<16, 1>(0) ^ chk.select<16, 1>(16) ^ chk.select<16, 1>(32) ^ chk.select<16, 1>(48)); });
}
static event k_seq_write(queue& q, uint32_t* dst, int T, int iters, uint32_t mb) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const uint32_t b = (uint32_t)i[0] * (uint32_t)iters; simd<uint32_t, 64> v(b, 1u);
    for (int it = 0; it < iters; ++it) { block_store<uint32_t, 64>(dst + (size_t)((b + it) & mb) * 64, v); v += 3u; } });
}
static event k_copy(queue& q, const uint32_t* src, uint32_t* dst, int T, int iters, uint32_t mb) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const uint32_t b = (uint32_t)i[0] * (uint32_t)iters;
    for (int it = 0; it < iters; ++it) { const size_t o = (size_t)((b + it) & mb) * 64; block_store<uint32_t, 64>(dst + o, block_load<uint32_t, 64>(src + o)); } });
}
static event k_strided(queue& q, const uint32_t* src, uint32_t* out, int T, int iters, uint32_t stride64, uint32_t m64) {   // units of 64 bytes
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    const uint32_t b = (uint32_t)i[0] * (uint32_t)iters; simd<uint32_t, 16> chk = 0;
    for (int it = 0; it < iters; ++it) chk ^= block_load<uint32_t, 16>(src + (size_t)(((b + it) * stride64) & m64) * 16);
    block_store<uint32_t, 16>(out + i[0] * 16, chk); });
}
static event k_gather(queue& q, const uint32_t* src, uint32_t* out, int T, int iters, uint32_t mask) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint32_t, 16> idx(0x9E3779B1u * ((uint32_t)i[0] + 1u), 0x85EBCA6Bu), acc = 0;
    for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += gather<uint32_t, 16>(src, (idx & mask) << 2u); }
    block_store<uint32_t, 16>(out + i[0] * 16, acc); });
}
static event k_nogather(queue& q, uint32_t* out, int T, int iters, uint32_t mask) {     // the index arithmetic alone
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint32_t, 16> idx(0x9E3779B1u * ((uint32_t)i[0] + 1u), 0x85EBCA6Bu), acc = 0;
    for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += (idx & mask) << 2u; }
    block_store<uint32_t, 16>(out + i[0] * 16, acc); });
}
static event k_scatter(queue& q, uint32_t* dst, int T, int iters, uint32_t mask) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint32_t, 16> idx(0x9E3779B1u * ((uint32_t)i[0] + 1u), 0x85EBCA6Bu);
    for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; scatter<uint32_t, 16>(dst, (idx & mask) << 2u, idx); } });
}
static event k_chase(queue& q, const uint32_t* src, uint32_t* out, int T, int iters, uint32_t mask) {
  return q.parallel_for(range<1>(T), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint32_t, 16> idx(0x9E3779B1u * ((uint32_t)i[0] + 1u), 0x85EBCA6Bu);
    for (int it = 0; it < iters; ++it) { const simd<uint32_t, 16> v = gather<uint32_t, 16>(src, (idx & mask) << 2u); idx = v * LA + LC; }
    block_store<uint32_t, 16>(out + i[0] * 16, idx); });
}
// SLM: MODE 0 sequential 256-byte block loads, 1 random gathers, 2 dependent gathers, 3 scalar-index-free baseline (no SLM access)
template <int SZ, int MODE>
event k_slm(queue& q, uint32_t* out, int T, int LS, int iters) {
  return q.parallel_for(nd_range<1>(T, LS), [=](nd_item<1> it_) SYCL_ESIMD_KERNEL {
    slm_init<SZ>();
    const uint32_t lid = it_.get_local_id(0), gid = (uint32_t)it_.get_global_id(0);
    const simd<uint32_t, 64> io(0u, 1u);
    for (uint32_t i = lid; i < SZ / 256; i += LS) slm_block_store<uint32_t, 64>(i * 256, io + i * 64u);
    barrier();
    const uint32_t mask = SZ / 4 - 1, mb = SZ / 256 - 1;
    simd<uint32_t, 16> idx(0x9E3779B1u * (gid + 1u), 0x85EBCA6Bu), acc = 0;
    if constexpr (MODE == 0) { simd<uint32_t, 64> chk = 0; const uint32_t b = gid * 7u;
      for (int it = 0; it < iters; ++it) chk ^= slm_block_load<uint32_t, 64>(((b + it) & mb) * 256);
      acc = chk.select<16, 1>(0) ^ chk.select<16, 1>(16) ^ chk.select<16, 1>(32) ^ chk.select<16, 1>(48); }
    else if constexpr (MODE == 1) for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += slm_gather<uint32_t, 16>((idx & mask) << 2u); }
    else if constexpr (MODE == 2) { for (int it = 0; it < iters; ++it) { const simd<uint32_t, 16> v = slm_gather<uint32_t, 16>((idx & mask) << 2u); idx = v * LA + LC; } acc = idx; }
    else for (int it = 0; it < iters; ++it) { idx = idx * LA + LC; acc += (idx & mask) << 2u; }
    block_store<uint32_t, 16>(out + gid * 16, acc); });
}

int main() {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "mem");
  const size_t GB = 1ull << 30, NSRC = GB / 4, NDST = GB / 8;
  uint32_t* src = malloc_device<uint32_t>(NSRC, q); uint32_t* dst = malloc_device<uint32_t>(NDST, q); uint32_t* out = malloc_device<uint32_t>(16 * 65536, q);
  { std::vector<uint32_t> h(NSRC); for (size_t i = 0; i < NSRC; ++i) h[i] = (uint32_t)i; q.memcpy(src, h.data(), GB).wait(); q.memset(dst, 0, GB / 2).wait(); }
  std::printf("section,test,working_set_bytes,threads,param,ms,GB_per_s,Maccess_per_s,ns_per_op_in_thread\n");
  auto row = [&](const char* sec, const char* test, size_t ws, int T, int param, double s, double bytes, double acc, double ops_in_thread) {
    std::printf("%s,%s,%zu,%d,%d,%.4f,%.1f,%.1f,%.2f\n", sec, test, ws, T, param, s * 1e3, bytes / s / 1e9, acc / s / 1e6, s * 1e9 / ops_in_thread); std::fflush(stdout); };
  const size_t wss[] = {4096, 16384, 65536, 262144, 1u << 20, 4u << 20, 16u << 20, 64u << 20, 256u << 20, GB};
  if (ab::want("ws")) for (size_t ws : wss) {
    const int T = 4096, it = 1024; const uint32_t mb = (uint32_t)(ws / 256 - 1), mask = (uint32_t)(ws / 4 - 1);
    const double bytes = (double)T * it * 256, acc = (double)T * it * 16;
    row("ws", "seq_read", ws, T, 0, ab::best_time(q, [&] { return k_seq_read(q, src, out, T, it, mb); }, true), bytes, 0, it);
    if (ws <= GB / 2) {
      row("ws", "seq_write", ws, T, 0, ab::best_time(q, [&] { return k_seq_write(q, dst, T, it, mb); }, true), bytes, 0, it);
      row("ws", "copy", ws, T, 0, ab::best_time(q, [&] { return k_copy(q, src, dst, T, it, mb); }, true), bytes, 0, it);
      row("ws", "scatter", ws, T, 0, ab::best_time(q, [&] { return k_scatter(q, dst, T, it, mask); }, true), acc * 4, acc, it);
    }
    row("ws", "gather", ws, T, 0, ab::best_time(q, [&] { return k_gather(q, src, out, T, it, mask); }, true), acc * 4, acc, it);
    row("ws", "chase_4096thr", ws, T, 0, ab::best_time(q, [&] { return k_chase(q, src, out, T, it, mask); }, true), acc * 4, acc, it);
    row("ws", "chase_1thr", ws, 1, 0, ab::best_time(q, [&] { return k_chase(q, src, out, 1, 20000, mask); }, true), 20000.0 * 64, 20000.0 * 16, 20000);
  }
  if (ab::want("ws")) row("ws", "index_arith_only", 0, 4096, 0, ab::best_time(q, [&] { return k_nogather(q, out, 4096, 1024, 0xffffu); }, true), 0, 4096.0 * 1024 * 16, 1024);
  if (ab::want("threads")) for (int T : {1, 8, 32, 64, 160, 320, 640, 960, 1280, 1920, 2560, 5120, 16384, 65536}) {
    const int it = T <= 64 ? 16384 : 2048; const double acc = (double)T * it * 16;
    row("threads", "seq_read", GB, T, 0, ab::best_time(q, [&] { return k_seq_read(q, src, out, T, it, (uint32_t)(GB / 256 - 1)); }, true), (double)T * it * 256, 0, it);
    row("threads", "seq_read", 65536, T, 0, ab::best_time(q, [&] { return k_seq_read(q, src, out, T, it, 255u); }, true), (double)T * it * 256, 0, it);
    row("threads", "gather", GB, T, 0, ab::best_time(q, [&] { return k_gather(q, src, out, T, it, (uint32_t)(GB / 4 - 1)); }, true), acc * 4, acc, it);
    row("threads", "gather", 16384, T, 0, ab::best_time(q, [&] { return k_gather(q, src, out, T, it, 4095u); }, true), acc * 4, acc, it);
    row("threads", "chase", GB, T, 0, ab::best_time(q, [&] { return k_chase(q, src, out, T, it, (uint32_t)(GB / 4 - 1)); }, true), acc * 4, acc, it);
  }
  if (ab::want("stride")) for (size_t ws : {(size_t)(256u << 20), GB}) for (int st : {1, 2, 4, 16, 64, 1024}) {
    const int T = 4096, it = 2048;
    row("stride", "strided_read_64B", ws, T, st * 64, ab::best_time(q, [&] { return k_strided(q, src, out, T, it, (uint32_t)st, (uint32_t)(ws / 64 - 1)); }, true), (double)T * it * 64, (double)T * it, it);
  }
  if (ab::want("data")) {      // does the read rate depend on the CONTENT of the memory?  (the other sections read src[i] = i)
    std::vector<uint32_t> h(NDST); uint64_t seed = 5;
    for (int kind = 0; kind < 4; ++kind) { const char* kn[] = {"content: random", "content: zero", "content: index (i)", "content: random 8-bit values"};
      for (size_t i = 0; i < NDST; ++i) h[i] = kind == 0 ? ab::rng(seed) : kind == 1 ? 0u : kind == 2 ? (uint32_t)i : (ab::rng(seed) & 0xffu);
      q.memcpy(dst, h.data(), NDST * 4).wait();
      for (size_t ws : {(size_t)(1u << 20), (size_t)(4u << 20), (size_t)(8u << 20), (size_t)(16u << 20), (size_t)(32u << 20), (size_t)(64u << 20), (size_t)(128u << 20), (size_t)(256u << 20), (size_t)(512u << 20)}) {
        const int T = 4096, it = 1024;
        row("data", (std::string("seq_read ") + kn[kind]).c_str(), ws, T, 0, ab::best_time(q, [&] { return k_seq_read(q, dst, out, T, it, (uint32_t)(ws / 256 - 1)); }, true), (double)T * it * 256, 0, it);
        row("data", (std::string("gather ") + kn[kind]).c_str(), ws, T, 0, ab::best_time(q, [&] { return k_gather(q, dst, out, T, it, (uint32_t)(ws / 4 - 1)); }, true), (double)T * it * 64, (double)T * it * 16, it);
      } }
  }
  if (ab::want("slm")) {
    auto slm = [&]<int SZ>() {
      for (int T : {32, 640, 1280, 4096}) { const int it = 4096;
        row("slm", "slm_seq_read", SZ, T, 32, ab::best_time(q, [&] { return k_slm<SZ, 0>(q, out, T, 32, it); }, true), (double)T * it * 256, 0, it);
        row("slm", "slm_gather", SZ, T, 32, ab::best_time(q, [&] { return k_slm<SZ, 1>(q, out, T, 32, it); }, true), (double)T * it * 64, (double)T * it * 16, it);
        row("slm", "slm_chase", SZ, T, 32, ab::best_time(q, [&] { return k_slm<SZ, 2>(q, out, T, 32, it); }, true), (double)T * it * 64, (double)T * it * 16, it);
        row("slm", "slm_none(index arith)", SZ, T, 32, ab::best_time(q, [&] { return k_slm<SZ, 3>(q, out, T, 32, it); }, true), 0, (double)T * it * 16, it); } };
    slm.template operator()<4096>(); slm.template operator()<16384>(); slm.template operator()<65536>(); slm.template operator()<131072>();
  }
  free(src, q); free(dst, q); free(out, q);
  return 0;
}
