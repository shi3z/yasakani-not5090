// SPDX-License-Identifier: Apache-2.0
// Registers, threads and occupancy on the B60 (default 128-register mode only).
//  live   : NR live simd<uint32,16> values (64 bytes each), all read and written in every loop trip -> where do spills start?
//  group  : the same arithmetic kernel with different work-group sizes
//  threads: ... with different numbers of threads (how many run at once)
//  slm    : ... with an SLM allocation per work-group (does SLM limit the number of concurrent work-groups?)
//  launch : cost of a kernel launch (empty kernel), synchronous and queued
// usage: regs     env ARCH_ONLY=live,group,threads,slm,launch   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;

template <int NR>
event k_live(queue& q, uint32_t* out, int threads, int iters) {
  return q.parallel_for(range<1>(threads), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<uint32_t, 16> r[NR];
    unroll<NR>([&](auto c) { r[decltype(c)::value] = simd<uint32_t, 16>((uint32_t)i[0] + decltype(c)::value, 7u); });
    for (int it = 0; it < iters; ++it) unroll<NR>([&](auto c) { constexpr int k = decltype(c)::value; r[k] = (r[k] + r[(k + 1) % NR]) ^ 0x9E3779B1u; });
    simd<uint32_t, 16> s = 0;
    unroll<NR>([&](auto c) { s += r[decltype(c)::value]; });
    block_store<uint32_t, 16>(out + i[0] * 16, s); });
}
// 16 live values; SZ bytes of SLM per work-group (0 = none)
template <int SZ>
event k_work(queue& q, uint32_t* out, int threads, int LS, int iters) {
  constexpr int NR = 16;
  return q.parallel_for(nd_range<1>(threads, LS), [=](nd_item<1> it_) SYCL_ESIMD_KERNEL {
    const uint32_t gid = (uint32_t)it_.get_global_id(0);
    if constexpr (SZ > 0) { slm_init<SZ>(); const uint32_t lid = it_.get_local_id(0);
      for (uint32_t j = lid; j < SZ / 256; j += LS) slm_block_store<uint32_t, 64>(j * 256, simd<uint32_t, 64>(j, 1u));
      barrier(); }
    simd<uint32_t, 16> r[NR];
    unroll<NR>([&](auto c) { r[decltype(c)::value] = simd<uint32_t, 16>(gid + decltype(c)::value, 7u); });
    for (int it = 0; it < iters; ++it) unroll<NR>([&](auto c) { constexpr int k = decltype(c)::value; r[k] = (r[k] + r[(k + 1) % NR]) ^ 0x9E3779B1u; });
    simd<uint32_t, 16> s = 0;
    unroll<NR>([&](auto c) { s += r[decltype(c)::value]; });
    if constexpr (SZ > 0) s += slm_block_load<uint32_t, 16>(0);
    block_store<uint32_t, 16>(out + gid * 16, s); });
}
static event k_empty_es(queue& q, uint32_t* out) { return q.parallel_for(range<1>(1), [=](id<1>) SYCL_ESIMD_KERNEL { block_store<uint32_t, 16>(out, simd<uint32_t, 16>(1u)); }); }
static event k_empty_sc(queue& q, uint32_t* out) { return q.parallel_for(range<1>(1), [=](id<1> i) { out[i[0]] = 1; }); }

int main() {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "regs");
  uint32_t* out = malloc_device<uint32_t>(16 * 65536, q);
  std::printf("section,param,value,threads,group_size,iters,ms,ns_per_update_in_thread,G_register_updates_per_s\n");
  auto row = [&](const char* sec, const char* p, int v, int T, int LS, int iters, int nr, double s) {
    std::printf("%s,%s,%d,%d,%d,%d,%.4f,%.3f,%.3f\n", sec, p, v, T, LS, iters, s * 1e3, s * 1e9 / ((double)iters * nr), (double)T * iters * nr / s / 1e9); std::fflush(stdout); };
  if (ab::want("live")) {
    auto live = [&]<int NR>() { for (int T : {160, 5120}) { const int iters = 400000 / NR;
      row("live", "live_simd16_values", NR, T, 0, iters, NR, ab::best_time(q, [&] { return k_live<NR>(q, out, T, iters); }, true, 0.03, 4)); } };
    live.template operator()<8>(); live.template operator()<16>(); live.template operator()<32>(); live.template operator()<48>(); live.template operator()<64>();
    live.template operator()<80>(); live.template operator()<96>(); live.template operator()<104>(); live.template operator()<112>(); live.template operator()<116>();
    live.template operator()<120>(); live.template operator()<124>(); live.template operator()<128>(); live.template operator()<144>(); live.template operator()<160>();
    live.template operator()<192>(); live.template operator()<256>();
  }
  const int IT = 20000;
  auto safe = [&](auto&& f) -> double { try { return ab::best_time(q, f, true, 0.03, 4); } catch (const sycl::exception& e) { std::printf("# exception: %s\n", e.what()); return -1; } };
  if (ab::want("group")) for (int LS : {1, 2, 4, 8, 16, 32, 64, 128, 256}) {
    const double s = safe([&] { return k_work<0>(q, out, 5120, LS, IT); }); if (s > 0) row("group", "work_group_size", LS, 5120, LS, IT, 16, s); }
  if (ab::want("threads")) for (int T : {1, 8, 32, 64, 128, 160, 320, 480, 640, 800, 960, 1120, 1280, 1440, 1600, 1920, 2560, 5120, 10240, 20480}) {
    const double s = safe([&] { return k_work<0>(q, out, T, 1, IT); }); if (s > 0) row("threads", "threads", T, T, 1, IT, 16, s); }
  if (ab::want("slm")) {
    auto slm = [&]<int SZ>() { for (int LS : {8, 32}) for (int T : {32, 320, 640, 1280, 2560, 5120}) {
      const double s = safe([&] { return k_work<SZ>(q, out, T, LS, IT); }); if (s > 0) row("slm", "slm_bytes_per_group", SZ, T, LS, IT, 16, s); } };
    slm.template operator()<0>(); slm.template operator()<4096>(); slm.template operator()<16384>(); slm.template operator()<32768>(); slm.template operator()<65536>(); slm.template operator()<131072>();
  }
  if (ab::want("launch")) {
    std::printf("section,kernel,mode,us_per_launch\n");
    for (int es = 0; es < 2; ++es) { auto l = [&] { return es ? k_empty_es(q, out) : k_empty_sc(q, out); };
      std::printf("launch,%s,submit+wait,%.2f\n", es ? "empty_esimd" : "empty_sycl", ab::best_time(q, l, true, 0.05, 5) * 1e6);
      std::printf("launch,%s,queued (one wait per batch),%.2f\n", es ? "empty_esimd" : "empty_sycl", ab::best_time(q, l, false, 0.05, 5) * 1e6); }
    { double best = 1e30; for (int r = 0; r < 20; ++r) { const double t0 = ab::now(); q.wait(); best = std::min(best, ab::now() - t0); } std::printf("launch,none,queue.wait() on an idle queue,%.2f\n", best * 1e6); }
    { std::vector<uint32_t> h(16); double best = 1e30; for (int r = 0; r < 50; ++r) { const double t0 = ab::now(); q.memcpy(h.data(), out, 64).wait(); best = std::min(best, ab::now() - t0); }
      std::printf("launch,none,64-byte device-to-host memcpy + wait,%.2f\n", best * 1e6); }
  }
  free(out, q);
  return 0;
}
