// Per-launch cost of the non-GEMM decode kernels, measured back to back (N launches, one wait), on the buffers a real pass left.
//   tests/np_prof [rows ...]      default rows: 1 8 16
// For every kernel: the real kernel, an empty ESIMD kernel of the same launch shape (launch + dispatch floor) and pieces of the
// work (loads only, FWHT, exp, divisions, ...) so that the time of a thread can be attributed.
#include "../src/attn.hpp"
#include "../src/engine.hpp"
#include "../src/head.hpp"
#include <chrono>
#include <cstdio>
#include <fstream>
using namespace yx; using namespace yx::fx;
static double now() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
template <class Fn> static double per_launch(sycl::queue& q, Fn&& fn, int n = 400) { for (int i = 0; i < 20; ++i) fn(); q.wait(); double best = 1e18;
  for (int r = 0; r < 5; ++r) { const double t0 = now(); for (int i = 0; i < n; ++i) fn(); q.wait(); best = std::min(best, (now() - t0) / n); } return best; }

// expf of N vectors, written stage by stage so that the N dependency chains are interleaved in the instruction stream
template <int N> YXI void v_expf_n(vf* x) {
#define EACH(...) unroll<N>([&](auto ii_) { constexpr int i = decltype(ii_)::value; __VA_ARGS__; })
  const float LN2HI = u2f(0x3f317200u), LN2LO = u2f(0x35bfbe8eu);
  vu ux[N], ax[N]; vi k[N]; vf t[N], hi[N], lo[N], r[N], tt[N], c[N], den[N], qn[N], y[N], res[N], sg[N];
  EACH(ux[i] = bits(x[i]));
  EACH(ax[i] = ux[i] & 0x7fffffffu);
  EACH(sg[i] = flt((ux[i] & 0x80000000u) | 0x3f000000u));
  EACH(t[i] = u2f(0x3fb8aa3bu) * x[i]);
  EACH(t[i] = t[i] + sg[i]);
  EACH(k[i] = t[i]);
  EACH(vi k1 = 1; k1.merge(vi(-1), ux[i] >= 0x80000000u); k[i].merge(k1, ax[i] < 0x3f851592u));
  EACH(k[i].merge(vi(0), !(ax[i] > 0x3eb17218u)));
  EACH(t[i] = k[i]);
  EACH(hi[i] = t[i] * LN2HI);
  EACH(lo[i] = t[i] * LN2LO);
  EACH(hi[i] = x[i] - hi[i]);
  EACH(r[i] = hi[i] - lo[i]);
  EACH(tt[i] = r[i] * r[i]);
  EACH(c[i] = tt[i] * u2f(0xbb355215u));
  EACH(c[i] = u2f(0x3e2aaa8fu) + c[i]);
  EACH(c[i] = tt[i] * c[i]);
  EACH(c[i] = r[i] - c[i]);
  EACH(den[i] = 2.f - c[i]; den[i].merge(c[i] - 2.f, k[i] == 0));
  EACH(tt[i] = r[i] * c[i]);
  EACH(qn[i] = vdiv(tt[i], den[i]));
  EACH(y[i] = lo[i] - qn[i]);
  EACH(y[i] = y[i] - hi[i]);
  EACH(y[i] = 1.f - y[i]);
  EACH(res[i] = y[i] * flt(vu(k[i] + 0x7f) << 23));
  EACH(res[i].merge((y[i] * 2.f) * u2f(0x7f000000u), k[i] == 128));
  EACH(res[i].merge((y[i] * flt(vu(k[i] + (0x7f + 100)) << 23)) * u2f(0x0d800000u), k[i] < -125));
  EACH(res[i].merge(1.f - (qn[i] - r[i]), k[i] == 0));
  EACH(res[i].merge(1.f + x[i], ax[i] < 0x39000000u));
  EACH(res[i].merge(vf(0.f), ux[i] > 0xc2cff1b5u));
  EACH(res[i].merge(vf(u2f(0x7f800000u)), (!(ux[i] >= 0x80000000u)) & (ax[i] >= 0x42b17218u)));
  EACH(res[i].merge(x[i] + x[i], ax[i] > 0x7f800000u));
  EACH(x[i] = res[i]);
#undef EACH
}
template <int N> YXI void v_expf_nodiv(vf* x) {
#define EACH(...) unroll<N>([&](auto ii_) { constexpr int i = decltype(ii_)::value; __VA_ARGS__; })
  const float LN2HI = u2f(0x3f317200u), LN2LO = u2f(0x35bfbe8eu);
  vu ux[N], ax[N]; vi k[N]; vf t[N], hi[N], lo[N], r[N], tt[N], c[N], den[N], qn[N], y[N], res[N], sg[N];
  EACH(ux[i] = bits(x[i]));
  EACH(ax[i] = ux[i] & 0x7fffffffu);
  EACH(sg[i] = flt((ux[i] & 0x80000000u) | 0x3f000000u));
  EACH(t[i] = u2f(0x3fb8aa3bu) * x[i]);
  EACH(t[i] = t[i] + sg[i]);
  EACH(k[i] = t[i]);
  EACH(vi k1 = 1; k1.merge(vi(-1), ux[i] >= 0x80000000u); k[i].merge(k1, ax[i] < 0x3f851592u));
  EACH(k[i].merge(vi(0), !(ax[i] > 0x3eb17218u)));
  EACH(t[i] = k[i]);
  EACH(hi[i] = t[i] * LN2HI);
  EACH(lo[i] = t[i] * LN2LO);
  EACH(hi[i] = x[i] - hi[i]);
  EACH(r[i] = hi[i] - lo[i]);
  EACH(tt[i] = r[i] * r[i]);
  EACH(c[i] = tt[i] * u2f(0xbb355215u));
  EACH(c[i] = u2f(0x3e2aaa8fu) + c[i]);
  EACH(c[i] = tt[i] * c[i]);
  EACH(c[i] = r[i] - c[i]);
  EACH(den[i] = 2.f - c[i]; den[i].merge(c[i] - 2.f, k[i] == 0));
  EACH(tt[i] = r[i] * c[i]);
  EACH(qn[i] = tt[i] * den[i]);
  EACH(y[i] = lo[i] - qn[i]);
  EACH(y[i] = y[i] - hi[i]);
  EACH(y[i] = 1.f - y[i]);
  EACH(res[i] = y[i] * flt(vu(k[i] + 0x7f) << 23));
  EACH(res[i].merge((y[i] * 2.f) * u2f(0x7f000000u), k[i] == 128));
  EACH(res[i].merge((y[i] * flt(vu(k[i] + (0x7f + 100)) << 23)) * u2f(0x0d800000u), k[i] < -125));
  EACH(res[i].merge(1.f - (qn[i] - r[i]), k[i] == 0));
  EACH(res[i].merge(1.f + x[i], ax[i] < 0x39000000u));
  EACH(res[i].merge(vf(0.f), ux[i] > 0xc2cff1b5u));
  EACH(res[i].merge(vf(u2f(0x7f800000u)), (!(ux[i] >= 0x80000000u)) & (ax[i] >= 0x42b17218u)));
  EACH(res[i].merge(x[i] + x[i], ax[i] > 0x7f800000u));
  EACH(x[i] = res[i]);
#undef EACH
}

// the MLP kernel cut after stage C (native division), registers stored so that the work is not removed:
//  1 rfac + up: load, convert   2 + fwht   3 + scale, rbf   4 + gate: load, convert   5 + fwht   6 + scale, rbf (g)   7 + silu(g) * up
//  8 + a8: fwht, rf16, max   9 + a8: divisions, int8 stores   10 + scales (barrier)
template <int C> static void mlp_cut(sycl::queue& q, int M, int MR, const MlpPost P, float* out) {
  q.parallel_for(sycl::nd_range<1>(M * 34, 34), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL { slm_init<256>();
    const int gid = (int)it.get_global_id(0), tok = gid / 34, blk = gid % 34; const size_t n0 = (size_t)tok * yx::F + blk * 512; DivNative d;
    const vf rf = v_rfac(P.S + tok * SST, P.eps, d), u0 = P.us[tok]; vf x[32], y[32]; float als = 0.f;
    v_yrot_n(y, P.au + n0, P.tu + blk * 512, u0, rf);
    if constexpr (C >= 2) v_fwht512(y);
    if constexpr (C >= 3) w512<float>(P.su + blk * 512, [&](auto ri, vf s1) { constexpr int r = decltype(ri)::value; y[r] = v_rbf((y[r] * K512) * s1); });
    if constexpr (C >= 4) v_yrot_n(x, P.ag + n0, P.tg + blk * 512, u0, rf);
    if constexpr (C >= 5) v_fwht512(x);
    if constexpr (C >= 6) unroll<8>([&](auto qi) { constexpr int q = decltype(qi)::value; simd<float, 64> gv = block_load<float, 64>(P.sg + blk * 512 + 64 * q), iv = block_load<float, 64>(P.si + blk * 512 + 64 * q);
      unroll<4>([&](auto ki) { constexpr int k = decltype(ki)::value; constexpr int r = 4 * q + k; const vf g1 = gv.template select<16, 1>(16 * k), i1 = iv.template select<16, 1>(16 * k);
        const vf g = v_rbf((x[r] * K512) * g1);
        if constexpr (C >= 7) x[r] = v_rbf(v_rbf(v_silu(g, d)) * y[r]) * i1; else x[r] = g * i1; }); });
    vf amax = 0.f; if constexpr (C >= 8) amax = v_a8_pre(x);
    if constexpr (C >= 9) als = v_a8_div(x, amax, P.qx, P.alpha, MR, tok, blk, 34, d);
    if constexpr (C >= 10) v_scales((uint32_t)it.get_local_id(0), 34, tok, M, MR, als, P.us, P.Aw);
    if constexpr (C < 9) { if constexpr (C >= 4) v_store512(out + (size_t)gid * 512, x); else v_store512(out + (size_t)gid * 512, y); out[(size_t)gid * 512] = amax[0]; } });
}

namespace yx { namespace fx {
// DivFast with the reason of every marked lane counted: [0] divisions (lanes), [1] b not normal, [2] q0 outside, [3] a too small, [4] third step better
struct DivProbe { vu n = 0u, c1 = 0u, c2 = 0u, c3 = 0u, c4 = 0u; };
YXI bool failed(const DivProbe&) { return false; }
YXI vf dv(DivProbe& p, vf a, vf b) { DivFast f; const vf q = dv(f, a, b); const vf q0 = a / b; const vu ua = bits(a), ub = bits(b), uq = bits(q0);
  const vf fa = flt(ua & 0x7fffffffu), fb = flt(ub & 0x7fffffffu), fq = flt(uq & 0x7fffffffu); const float LO = u2f(0x01800000u), HI = u2f(0x7d800000u), LA = u2f(0x10000000u); const vm az = fa == 0.f;
  const vm b1 = !((fb >= LO) & (fb <= HI)), b2 = !az & !((fq >= LO) & (fq <= HI)), b3 = !az & !((fa >= LA) & (fa <= HI));
  p.n += 1u; p.c1.merge(p.c1 + 1u, b1); p.c2.merge(p.c2 + 1u, b2 & !b1); p.c3.merge(p.c3 + 1u, b3 & !b2 & !b1); p.c4.merge(p.c4 + 1u, (f.bad != 0u) & !b1 & !b2 & !b3);
  return div_ieee<float, 16>(a, b); }
} }
template <int W> static void mlp_probe(sycl::queue& q, int M, int MR, const yx::fx::MlpPost P, uint32_t* cnt) {
  q.parallel_for(sycl::nd_range<1>(M * 34, 34), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL {
    const int gid = (int)it.get_global_id(0), tok = gid / 34, blk = gid % 34; const size_t n0 = (size_t)tok * yx::F + blk * 512; DivProbe d;
    const vf rf = v_rfac(P.S + tok * SST, P.eps, d), u0 = P.us[tok]; vf x[32], y[32];
    if constexpr (W == 0) { v_yrot_n(y, P.au + n0, P.tu + blk * 512, u0, rf); v_fwht512(y);
      w512<float>(P.su + blk * 512, [&](auto ri, vf s1) { constexpr int r = decltype(ri)::value; y[r] = v_rbf((y[r] * K512) * s1); });
      v_yrot_n(x, P.ag + n0, P.tg + blk * 512, u0, rf); v_fwht512(x);
      w512<float>(P.sg + blk * 512, [&](auto ri, vf g1) { constexpr int r = decltype(ri)::value; x[r] = v_rbf((x[r] * K512) * g1); });
      unroll<8>([&](auto qi) { v_silu_n<4>(x + 4 * decltype(qi)::value, d); }); }
    else { v_yrot_n(x, P.ag + n0, P.tg + blk * 512, u0, rf); v_fwht512(x); const vf amax = v_a8_pre(x); float al[40]; int8_t qq[1]; (void)qq;
      const vf alv = dv(d, vf(hmax<float>(amax)), vf(127.f)); unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = dv(d, x[r], alv); }); (void)al; }
    vu o = 0u; uint32_t s[5] = {0, 0, 0, 0, 0};
    unroll<16>([&](auto li) { constexpr int l = decltype(li)::value; s[0] += (uint32_t)d.n[l]; s[1] += (uint32_t)d.c1[l]; s[2] += (uint32_t)d.c2[l]; s[3] += (uint32_t)d.c3[l]; s[4] += (uint32_t)d.c4[l]; });
    unroll<5>([&](auto ki) { constexpr int k = decltype(ki)::value; o.template select<1, 1>(k) = s[k]; });
    o.template select<1, 1>(5) = (s[1] | s[2] | s[3] | s[4]) ? 1u : 0u; o.template select<1, 1>(6) = 1u;
    atomic_update<atomic_op::add, uint32_t, 16>(cnt, vu(0u, 4u), o, vi(0, 1) < 7); });
}
// test kernels: P = piece
//  0 empty  1 load 512 int64 + tau, convert (yrot_n)  2 = 1 + fwht512  3 = 2 + scale/rbf  4 two inputs up to the silu product (no a8)
//  5 a8 tail only (on a loaded float block)  6 32 x expf  7 32 x div_ieee  8 32 x rbf  9 32 x rf16  10 fwht512 only (float block)
template <int P, int LS = 0> static void piece(sycl::queue& q, int n_thr, int G, const int64_t* acc, const float* tau, const float* so, const float* S, const float* us, float* out, int8_t* qx, float* alpha) {
  q.parallel_for(sycl::nd_range<1>(n_thr, LS ? LS : G), [=](sycl::nd_item<1> it) SYCL_ESIMD_KERNEL { const int gid = (int)it.get_global_id(0), tok = gid / G, blk = gid % G; vf x[32];
    if constexpr (P == 0) { return; }
    if constexpr (P >= 1 && P <= 4) { const vf rf = v_rfac(S + tok * SST, 1e-6f); v_yrot_n(x, acc + (size_t)tok * yx::F + blk * 512, tau + blk * 512, vf(us[tok]), rf); }
    if constexpr (P >= 5) v_load512(x, so + (blk % 10) * 512);
    if constexpr (P >= 2 && P <= 4) v_fwht512(x);
    if constexpr (P == 10) v_fwht512(x);
    if constexpr (P >= 3 && P <= 4) w512<float>(so + blk * 512, [&](auto ri, vf s1) { constexpr int r = decltype(ri)::value; x[r] = v_rbf((x[r] * K512) * s1); });
    if constexpr (P == 4) { vf y[32]; const vf rf = v_rfac(S + tok * SST, 1e-6f); v_yrot_n(y, acc + (size_t)tok * yx::F + blk * 512, tau + blk * 512, vf(us[tok]), rf); v_fwht512(y);
      w512<float>(so + blk * 512, [&](auto ri, vf s1) { constexpr int r = decltype(ri)::value; y[r] = v_rbf((y[r] * K512) * s1); x[r] = v_rbf(v_rbf(v_silu(x[r])) * y[r]) * s1; }); }
    if constexpr (P == 5) { v_a8_tail(x, qx, alpha, 16, tok, blk, G); return; }
    if constexpr (P == 6) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = v_expf(x[r]); });
    if constexpr (P == 11) unroll<8>([&](auto gi) { v_expf_n<4>(x + 4 * decltype(gi)::value); });
    if constexpr (P == 12) unroll<4>([&](auto gi) { v_expf_n<8>(x + 8 * decltype(gi)::value); });
    if constexpr (P == 13) v_expf_n<16>(x), v_expf_n<16>(x + 16);
    if constexpr (P == 14) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf v = x[r]; unroll<20>([&](auto) { v = v * 1.0001f + 0.5f; }); x[r] = v; });
    if constexpr (P == 15) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf v = x[r]; unroll<20>([&](auto) { v = flt(bits(v * 1.0001f) + 1u); }); x[r] = v; });
    if constexpr (P == 16) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf v = x[r]; unroll<20>([&](auto) { vf w = v * 1.0001f; w.merge(v + 0.5f, v < 0.25f); v = w; }); x[r] = v; });
    if constexpr (P == 17) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf v = x[r]; unroll<20>([&](auto) { vf w = v * 1.0001f; w.merge(v + 0.5f, bits(v) < 0x3e800000u); v = w; }); x[r] = v; });
    if constexpr (P == 18) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vf v = x[r]; unroll<20>([&](auto) { vi k = v; v = v * 1.0001f + vf(k); }); x[r] = v; });
    if constexpr (P == 19) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; vu v = bits(x[r]); unroll<40>([&](auto) { v = (v + 0x7fffu) ^ (v >> 3); }); x[r] = flt(v); });
    if constexpr (P == 20) unroll<8>([&](auto gi) { v_expf_nodiv<4>(x + 4 * decltype(gi)::value); });
    if constexpr (P == 21) { vf d[32]; v_yrot_n(d, acc + (size_t)tok * yx::F + blk * 512, tau + blk * 512, vf(us[tok]), vf(1.f)); unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = vdiv(x[r] + 0.37f, d[r] + 0.11f); }); }
    if constexpr (P == 22) { vf d[32]; v_yrot_n(d, acc + (size_t)tok * yx::F + blk * 512, tau + blk * 512, vf(us[tok]), vf(1.f)); unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = (x[r] + 0.37f) * (d[r] + 0.11f); }); }
    if constexpr (P == 23) { vf d[32]; v_yrot_n(d, acc + (size_t)tok * yx::F + blk * 512, tau + blk * 512, vf(us[tok]), vf(1.f)); unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = (x[r] + 0.37f) / (d[r] + 0.11f); }); }
    if constexpr (P == 7) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = vdiv(x[r], x[(r + 1) & 31] + 3.f); });
    if constexpr (P == 8) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = v_rbf(x[r]); });
    if constexpr (P == 9) unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = v_rf16(x[r]); });
    v_store512(out + (size_t)gid * 512, x); });
}
int main(int argc, char** argv) {
  std::vector<int> rows; for (int i = 1; i < argc; ++i) rows.push_back(atoi(argv[i])); if (rows.empty()) rows = {1, 8, 16};
  std::ifstream f("work/ids.bin", std::ios::binary); std::vector<int> base(12); f.read((char*)base.data(), 48);
  Engine E("models/speed/qwen38_27b_speed.yskn", 8, 16); sycl::queue& q = E.q;      // 8 layers are enough: layer 3 is an attention layer
  std::vector<int> ids(16); for (int i = 0; i < 16; ++i) ids[i] = base[i % 12]; q.memcpy(E.ids, ids.data(), 64).wait();
  auto pass = [&](int M) { const float* sg = E.first_sg(0); E.embed(M); E.res_norm(nullptr, M, E.L[0].ln1, sg, true);
    for (int l = 0; l < 3; ++l) { Layer& Y = E.L[l]; E.gdn(Y.g, M, 0); E.res_norm(&Y.g.out, M, Y.ln2, Y.m.gate.sg_in, false); E.mlp(Y.m, M); E.res_norm(&Y.m.down, M, E.L[l + 1].ln1, sg, l + 1 != 3); } };
  float* out = sycl::malloc_device<float>((size_t)16 * 128 * 512, q); const float eps = E.eps;
  for (int M : rows) { const int MR = Engine::mr_of(M); pass(M); q.wait(); Layer& Y = E.L[2]; Gdn& G = Y.g; Mlp& Mm = Y.m;
    std::printf("rows %d (microseconds per launch, back to back)\n", M);
    auto row = [&](const char* n, double t, double floor) { std::printf("  %-44s %7.1f   (above the empty launch: %6.1f)\n", n, t, t - floor); };
    // launch floors
    const double e10 = per_launch(q, [&] { piece<0>(q, M * 10, 10, Mm.gate.part, Mm.gate.tau, Mm.gate.sg_out, E.S, E.us, out, E.qx, E.alpha); });
    const double e34 = per_launch(q, [&] { piece<0>(q, M * 34, 34, Mm.gate.part, Mm.gate.tau, Mm.gate.sg_out, E.S, E.us, out, E.qx, E.alpha); });
    const double e128 = per_launch(q, [&] { piece<0>(q, M * 128, 1, Mm.gate.part, Mm.gate.tau, Mm.gate.sg_out, E.S, E.us, out, E.qx, E.alpha); });
    std::printf("  empty launch: %d threads %.1f, %d threads %.1f, %d threads %.1f\n", M * 10, e10, M * 34, e34, M * 128, e128);
    // the kernels (inputs: what the pass left; outputs go to the scratch buffers they normally write)
    row("res_norm (after a GEMM)", per_launch(q, [&] { k_res_norm(q, M, MR, E.h, E.S, E.alpha, E.qx, E.xn, Mm.down.part, Mm.down.tau, Mm.down.sg_out, E.us, E.Aw, Y.ln2, Mm.gate.sg_in); }), e10);
    row("mlp post", per_launch(q, [&] { k_mlp_post(q, M, MR, MlpPost{Mm.gate.part, Mm.up.part, Mm.gate.tau, Mm.up.tau, Mm.gate.sg_out, Mm.up.sg_out, Mm.down.sg_in, E.S, E.us, E.alpha, E.qx, E.Aw, eps}); }), e34);
    row("gdn post", per_launch(q, [&] { k_gdn_post(q, M, GdnPost{G.qkv.part, G.z.part, G.qkv.tau, G.z.tau, G.qkv.sg_out, G.z.sg_out, E.us, E.S, G.wa, G.wb, G.A_log, G.dt_bias, E.xn, G.mixed, E.z, E.ab, G.beta, G.g, eps}); }), e128);
    row("gdn conv", per_launch(q, [&] { k_conv(q, M, G.mixed, G.conv, G.hist, G.q, G.k, G.v); }), e128);
    row("gdn state (look-ahead, no commit)", per_launch(q, [&] { k_state(q, M, 0, G.q, G.k, G.v, G.beta, G.g, G.S, E.Sscr, E.core, G.mixed, G.hist); }), e128);
    row("gdn out", per_launch(q, [&] { k_gdn_out(q, M, MR, E.core, E.z, G.gnorm, G.out.sg_in, eps, E.alpha, E.qx, E.us, E.Aw); }), e10);
    { const MlpPost MP{Mm.gate.part, Mm.up.part, Mm.gate.tau, Mm.up.tau, Mm.gate.sg_out, Mm.up.sg_out, Mm.down.sg_in, E.S, E.us, E.alpha, E.qx, E.Aw, eps}; double prev = e34;
      const char* cn[] = {"", "rfac + up: load, convert", "+ fwht", "+ scale, rbf", "+ gate: load, convert", "+ fwht", "+ scale, rbf", "+ silu(g) * up", "+ a8: fwht, rf16, max", "+ a8: divisions, int8 stores", "+ scales (barrier, thread 0)"};
#define MC(C) { const double t = per_launch(q, [&] { mlp_cut<C>(q, M, MR, MP, out); }); std::printf("  mlp post (native division) cut %2d %-30s %7.1f   (+%.1f)\n", C, cn[C], t, t - prev); prev = t; }
      MC(1) MC(2) MC(3) MC(4) MC(5) MC(6) MC(7) MC(8) MC(9) MC(10) }
    { const MlpPost MP{Mm.gate.part, Mm.up.part, Mm.gate.tau, Mm.up.tau, Mm.gate.sg_out, Mm.up.sg_out, Mm.down.sg_in, E.S, E.us, E.alpha, E.qx, E.Aw, eps};
      uint32_t* cnt = sycl::malloc_device<uint32_t>(16, q); uint32_t c[7];
      for (int w = 0; w < 2; ++w) { q.memset(cnt, 0, 64).wait(); if (w == 0) mlp_probe<0>(q, M, MR, MP, cnt); else mlp_probe<1>(q, M, MR, MP, cnt); q.wait(); q.memcpy(c, cnt, 28).wait();
        std::printf("  exact-division probe, %s: %u lane divisions; b not normal %u, q0 out of range %u, a too small %u, third step %u; threads with a marked lane %u of %u\n",
                    w == 0 ? "rfac + silu" : "a8 divisions", c[0], c[1], c[2], c[3], c[4], c[5], c[6]); } }
    // pieces, 34 threads per row (the MLP shape)
    const char* pn[] = {"", "piece: load int64 + tau, convert, store", "piece: + fwht512", "piece: + scale, rbf", "piece: two inputs, silu product (no a8)", "piece: load, a8 tail",
                        "piece: load, 32 x expf, store", "piece: load, 32 x div_ieee, store", "piece: load, 32 x rbf, store", "piece: load, 32 x rf16, store", "piece: load, fwht512, store", "piece: load, 32 x expf interleaved by 4, store", "piece: load, 32 x expf interleaved by 8, store", "piece: load, 32 x expf interleaved by 16, store", "chain: 32 x 20 (mul, add) float only", "chain: 32 x 20 (mul float, add int on the bits)", "chain: 32 x 20 (mul, add, float cmp, predicated mov)", "chain: 32 x 20 (mul, add, int cmp on bits, predicated mov)", "chain: 32 x 20 (float->int, int->float, mul, add)", "chain: 32 x 40 (add, shr, xor) int only", "32 x expf by 4 with the division replaced by a product", "32 x div_ieee on real data", "the same with a product instead", "the same with operator/ (not IEEE)"};
#define PC(P) row(pn[P], per_launch(q, [&] { piece<P>(q, M * 34, 34, Mm.gate.part, Mm.gate.tau, Mm.gate.sg_out, E.S, E.us, out, E.qx, E.alpha); }), e34);
#define PL(P, LS) { char nm[96]; std::snprintf(nm, 96, "%s [work-group of %d]", pn[P], LS); row(nm, per_launch(q, [&] { piece<P, LS>(q, M * 34, 34, Mm.gate.part, Mm.gate.tau, Mm.gate.sg_out, E.S, E.us, out, E.qx, E.alpha); }), e34); }
    PL(4, 1) PL(4, 2) PL(4, 17) PL(5, 1) PL(6, 1) PL(21, 1)
    PC(1) PC(2) PC(3) PC(4) PC(5) PC(6) PC(11) PC(14) PC(15) PC(16) PC(17) PC(18) PC(19) PC(20) PC(21) PC(22) PC(23) PC(7) PC(8) PC(9) PC(10)
    pass(M); q.wait(); }
}
