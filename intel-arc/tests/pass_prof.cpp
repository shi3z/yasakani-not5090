// Whole-pass timing (64 layers + head) at a given context length, with a per-component breakdown.
#include "../src/attn.hpp"
#include "../src/engine.hpp"
#include "../src/head.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
using namespace yx;

// res_norm cut after stage C (test: where the time of one thread goes, on the data a real pass left; outputs go to scratch)
//  1 acc + tau loads, convert   2 + fwht   3 + h and scale loads, residual add, store   4 + sum of squares
//  5 + weight / sign loads, bf16 store   6 + a8: fwht, fp16 rounding, max   7 + a8: divisions, int8 stores   8 + scales (barrier)
template <int C> static void res_norm_cut(sycl::queue& q, int M, int MR, const float* h, float* S, float* alpha, int8_t* qx, uint16_t* xu, const int64_t* acc, const float* tau, const float* so,
                                          float* us, int32_t* Aw, const float* w, const float* sg_in, float* out) {
  using namespace yx::fx;
  q.parallel_for(sycl::nd_range<1>(M * 10, 10), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL { slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 10, blk = gid % 10; const float* hp = h + (size_t)tok * H + blk * 512; float* op = out + (size_t)gid * 512; vf x[32]; float als = 0.f; vf amax = 0.f;
    v_yrot(x, acc + (size_t)tok * H + blk * 512, tau + blk * 512, vf(us[tok]));
    if constexpr (C >= 2) v_fwht512(x);
    if constexpr (C >= 3) unroll<8>([&](auto qi) { constexpr int q = decltype(qi)::value; simd<float, 64> hv = block_load<float, 64>(hp + 64 * q), sv = block_load<float, 64>(so + blk * 512 + 64 * q); simd<float, 64> ov;
        unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; constexpr int r = 4 * q + k; const vf h1 = hv.template select<16, 1>(16 * k), s1 = sv.template select<16, 1>(16 * k);
          x[r] = v_rbf(h1 + v_rbf((x[r] * K512) * s1)); ov.template select<16, 1>(16 * k) = x[r]; });
        block_store<float, 64>(op + 64 * q, ov); });
    if constexpr (C >= 4) { vf a[8]; unroll<8>([&](auto qi) { constexpr int k = decltype(qi)::value; vf s = 0.f;
        unroll<4>([&](auto ji) { constexpr int j = decltype(ji)::value; s = s + x[k + 8 * j] * x[k + 8 * j]; }); a[k] = s; }); S[tok * SST + blk] = v_tree128(a); }
    if constexpr (C >= 5) unroll<8>([&](auto qi) { constexpr int q = decltype(qi)::value; simd<float, 64> wv = block_load<float, 64>(w + blk * 512 + 64 * q), gv = block_load<float, 64>(sg_in + blk * 512 + 64 * q); simd<uint16_t, 64> uv;
        unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; constexpr int r = 4 * q + k; const vf w1 = wv.template select<16, 1>(16 * k), g1 = gv.template select<16, 1>(16 * k);
          const vf u = x[r] * (1.f + w1); uv.template select<16, 1>(16 * k) = simd<uint16_t, 16>(v_f2bf(u)); x[r] = u * g1; });
        block_store<uint16_t, 64>(xu + (size_t)tok * H + blk * 512 + 64 * q, uv); });
    if constexpr (C >= 6) amax = v_a8_pre(x);
    if constexpr (C == 7 || C == 8) { DivIeee d; als = v_a8_div(x, amax, qx, alpha, MR, tok, blk, 10, d); }
    if constexpr (C == 9) { DivFast d; als = v_a8_div(x, amax, qx, alpha, MR, tok, blk, 10, d); if (failed(d)) out[(size_t)M * 5120 - 1 - gid] = 1.f; }      // counts nothing, only keeps the check alive
    if constexpr (C == 10) { DivNative d; als = v_a8_div(x, amax, qx, alpha, MR, tok, blk, 10, d); }
    if constexpr (C == 11) { DivFast d; als = v_a8_div(x, amax, qx, alpha, MR, tok, blk, 10, d); if (failed(d)) { DivIeee e; als = v_a8_div(x, amax, qx, alpha, MR, tok, blk, 10, e); atomic_update<atomic_op::inc, uint32_t, 16>((uint32_t*)Aw + 60000, vu(0u), vi(0, 1) < 1); } }
    if constexpr (C == 13) als = v_a8_cert(x, amax, qx, alpha, MR, tok, blk, 10);
    if constexpr (C == 12) { const vf al = vdiv(vf(hmax<float>(amax)), vf(127.f)); als = al[0]; alpha[tok * 10 + blk] = als; }      // only the scale: no int8 output
    if constexpr (C == 8) v_scales((uint32_t)it.get_local_id(0), 10, tok, M, MR, als, us + 4096, Aw);
    if constexpr ((C < 7 && C != 3 && C != 5) || C == 12) { v_store512(op, x); if constexpr (C == 6) op[0] = amax[0]; }
    if constexpr (C == 5) { simd<float, 16> t = x[0]; unroll<31>([&](auto ri) { t = t + x[decltype(ri)::value + 1]; }); block_store<float, 16>(op, t); } });
}
int main(int argc, char** argv) {
  const int ctx = argc > 1 ? atoi(argv[1]) : 256, max_ctx = argc > 2 ? atoi(argv[2]) : 8192;
  std::ifstream f("work/ids.bin", std::ios::binary); std::vector<int> base(12); f.read((char*)base.data(), 48);
  Engine E("models/speed/qwen38_27b_speed.yskn", NL); Head hd; hd.init(E);
  std::vector<std::unique_ptr<Attn>> A; for (int l = 3; l < NL; l += 4) A.emplace_back(std::make_unique<Attn>(E, l, max_ctx)); E.q.wait();
  std::map<std::string, double> T; bool prof = false; int pos = 0;
  auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  auto tm = [&](const char* n, auto&& fn) { if (!prof) { fn(); return; } const double t0 = now(); fn(); E.q.wait(); T[n] += now() - t0; };
  auto pass = [&](int M, bool commit) { const float* sg = E.first_sg(0);
    tm("embed+res_norm", [&] { E.embed(M); E.res_norm(nullptr, M, E.L[0].ln1, sg, !E.L[0].full); });
    for (int l = 0; l < NL; ++l) { Layer& Y = E.L[l];
      if (Y.full) { Attn& a = *A[l / 4]; tm("attention (16 layers)", [&] { attn_forward(E, a, M, pos); }); tm("res_norm x128", [&] { E.res_norm(&a.o, M, Y.ln2, Y.m.gate.sg_in, false); }); }
      else { tm("gdn (48 layers)", [&] { E.gdn(Y.g, M, commit ? M : 0); }); tm("res_norm x128", [&] { E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); }); }
      tm("mlp (64 layers)", [&] { E.mlp(Y.m, M); });
      tm("res_norm x128", [&] { if (l + 1 < NL) E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, sg, !E.L[l + 1].full); else E.res_norm(&Y.m.down, M, hd.fnorm, hd.T.sg_in, false); }); } };
  std::vector<int> ids(512); int am[16];
  { const int B = E.cap; for (; pos + B <= ctx; pos += B) { for (int i = 0; i < B; ++i) ids[i] = base[(pos + i) % 12]; E.q.memcpy(E.ids, ids.data(), B * 4).wait(); pass(B, true); } }     // context with the prefill batch
  E.q.wait(); std::printf("context %d\n", pos);
  const int only = getenv("YX_PP_M") ? atoi(getenv("YX_PP_M")) : 0;          // YX_PP_M=256: only this row count
  // YX_PP_ATTN="0,1,2,...": for the row count YX_PP_M, time the attention stage of a pass with each attention experiment (median of 5, with waits)
  if (getenv("YX_PP_ATTN") && only) { const int M = only; for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    std::string lst = getenv("YX_PP_ATTN"); size_t a0 = 0;
    while (a0 < lst.size()) { size_t a1 = lst.find(',', a0); if (a1 == std::string::npos) a1 = lst.size(); g_attn_mode = atoi(lst.substr(a0, a1 - a0).c_str()); a0 = a1 + 1;
      std::vector<double> ts, tk, tp; for (int r = 0; r < 6; ++r) { E.prof = true; E.T.clear(); E.mark(nullptr); const double f0 = now(); pass(M, false); E.q.wait(); const double f1 = now(); E.prof = false;
        double t = 0; for (auto& kv : E.T) if (kv.first.find("scores") != std::string::npos || kv.first.find("values") != std::string::npos || kv.first.find("fused") != std::string::npos) t += kv.second;
        if (r) { ts.push_back(t); tp.push_back(f1 - f0); } }
      std::sort(ts.begin(), ts.end()); std::sort(tp.begin(), tp.end());
      std::printf("attention experiment %2d  context %6d  rows %d:  scores + values of 16 layers %8.2f ms (median of 5; min %.2f, max %.2f)   whole pass with waits %.1f ms\n", g_attn_mode, pos, M, ts[2], ts[0], ts[4], tp[2]); std::fflush(stdout); }
    return 0; }
  if (getenv("YX_PP_CUT") && only) { const int M = only, MR = Engine::mr_of(M); for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait(); pass(M, false); E.q.wait();
    Layer& Y = E.L[NL - 1]; float* out = sycl::malloc_device<float>((size_t)M * 10 * 512, E.q); uint16_t* xu2 = sycl::malloc_device<uint16_t>((size_t)M * H, E.q); float* us2 = sycl::malloc_device<float>(8192, E.q);
    E.q.memcpy(us2, E.us, M * 4).wait(); sycl::queue& q = E.q;
    auto per = [&](auto&& fn) { for (int i = 0; i < 20; ++i) fn(); q.wait(); std::vector<double> t; for (int r = 0; r < 7; ++r) { const double t0 = now(); for (int i = 0; i < 300; ++i) fn(); q.wait(); t.push_back((now() - t0) / 300 * 1000); } std::sort(t.begin(), t.end()); return t[3]; };
    const double e0 = per([&] { q.parallel_for(sycl::nd_range<1>(M * 10, 10), [=](sycl::nd_item<1>) SYCL_ESIMD_KERNEL {}); }); std::printf("rows %d: empty launch of the res_norm shape %.1f us (median of 7 x 300 launches)\n", M, e0); double prev = e0;
    const char* cn[] = {"", "acc + tau loads, convert", "+ fwht", "+ h, scale loads, add, store", "+ sum of squares", "+ weight, sign loads, bf16 store", "+ a8: fwht, fp16, max", "+ a8: divisions, int8 stores", "+ scales (barrier)"};
#define RC(C) { const double t = per([&] { res_norm_cut<C>(q, M, MR, E.h, E.S, E.alpha, E.qx, xu2, Y.m.down.part, Y.m.down.tau, Y.m.down.sg_out, us2, E.Aw, hd.fnorm, hd.T.sg_in, out); }); \
      std::printf("  res_norm cut %d %-34s %6.1f us  (+%.1f)\n", C, cn[C], t, t - prev); prev = t; }
    RC(1) RC(2) RC(3) RC(4) RC(5) RC(6) RC(7) RC(8)
    { uint32_t* cnt = (uint32_t*)E.Aw + 60000; (void)cnt; const double t6 = per([&] { res_norm_cut<6>(q, M, MR, E.h, E.S, E.alpha, E.qx, xu2, Y.m.down.part, Y.m.down.tau, Y.m.down.sg_out, us2, E.Aw, hd.fnorm, hd.T.sg_in, out); });
      const char* vn[] = {"div_ieee (strict)", "", "DivFast, no repeat", "native divide", "DivFast + repeat with div_ieee when marked", "scale only, no int8 output (stores x)", "certified integer (strict), div_ieee when not proven"}; const int vc[] = {7, 0, 9, 10, 11, 12, 13};
#define RV(I, C) { const double t = per([&] { res_norm_cut<C>(q, M, MR, E.h, E.S, E.alpha, E.qx, xu2, Y.m.down.part, Y.m.down.tau, Y.m.down.sg_out, us2, E.Aw, hd.fnorm, hd.T.sg_in, out); }); \
        std::printf("  a8 divisions + int8 stores with %-44s %6.1f us  (+%.1f over cut 6)\n", vn[I], t, t - t6); (void)vc; }
      RV(0, 7) RV(2, 9) RV(3, 10) RV(4, 11) RV(5, 12) RV(6, 13) }
    return 0; }
  for (int M : {1, 8, 16, 32, 64, 128, 256, 512}) { if (M > E.cap) break; if (only && M != only) continue; for (int i = 0; i < M; ++i) ids[i] = base[i % 12]; E.q.memcpy(E.ids, ids.data(), M * 4).wait();
    double best = 1e9, bh = 1e9;
    for (int r = 0; r < 6; ++r) { double t0 = now(); pass(M, false); E.q.wait(); double t1 = now(); if (M <= 16) hd.run(E, M, 0, M, am); double t2 = now(); best = std::min(best, t1 - t0); bh = std::min(bh, t2 - t1); }
    prof = true; T.clear(); pass(M, false); prof = false;
    if (getenv("YX_FINE")) { E.prof = true; E.T.clear(); E.mark(nullptr); const double f0 = now(); pass(M, false); E.q.wait(); const double f1 = now(); E.prof = false;
      std::printf("  fine profile M=%d (pass with waits %.1f ms):\n", M, f1 - f0); for (auto& kv : E.T) std::printf("    %-28s %8.2f ms  %5.1f%%\n", kv.first.c_str(), kv.second, 100 * kv.second / (f1 - f0)); }
    std::printf("M=%2d: pass %.1f ms + head (all rows, arg-max) %.2f ms;  breakdown with waits:", M, best, bh);
    double s = 0; for (auto& kv : T) { std::printf("  %s %.1f", kv.first.c_str(), kv.second); s += kv.second; } std::printf("  (sum %.1f)\n", s); }
}
