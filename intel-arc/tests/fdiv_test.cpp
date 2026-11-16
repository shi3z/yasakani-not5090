// Exact float division without div_ieee:  v_fdiv (src/fast.hpp) against div_ieee, bit for bit, on many operand classes;
// and the error of the native divide in ulp.   tests/fdiv_test [millions of pairs per class, default 64]
#include "../src/fast.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace yx; using namespace yx::fx;
int main(int argc, char** argv) {
  const size_t n = (size_t)(argc > 1 ? atoi(argv[1]) : 64) << 20; sycl::queue q{sycl::gpu_selector_v, sycl::property::queue::in_order()};     // as the engine: a kernel starts when the previous one is done
  uint32_t *da = sycl::malloc_device<uint32_t>(n, q), *db = sycl::malloc_device<uint32_t>(n, q); uint32_t* cnt = sycl::malloc_device<uint32_t>(64, q);
  std::vector<uint32_t> a(n), b(n); std::mt19937_64 g(12345); size_t total_bad = 0;
  auto U = [&] { return (uint32_t)(g() >> 32); };
  const char* names[] = {"any bit patterns (all exponents, zeros, subnormals, inf, NaN)", "normal numbers, exponents -40..40", "same exponent (quotients near 1)", "numerator = k x denominator (exact quotients)",
                         "small integers", "quotients next to powers of two", "tiny numerators (expf: r*c / den)", "x / (1 + e^-x) operands", "x / (max / 127)", "denominator a power of two",
                         "subnormal results", "results near overflow"};
  for (int cls = 0; cls < 12; ++cls) {
    for (size_t i = 0; i < n; ++i) { uint32_t x, y; float fx_, fy_;
      auto norm = [&](int lo, int hi) { return (U() & 0x807fffffu) | ((uint32_t)(127 + lo + (int)(U() % (uint32_t)(hi - lo + 1))) << 23); };
      switch (cls) {
        case 0: x = U(); y = U(); if ((i & 15) == 0) x &= 0x80000000u; if ((i & 15) == 1) y &= 0x80000000u; if ((i & 15) == 2) x = (x & 0x80000000u) | 0x7f800000u; if ((i & 15) == 3) y = (y & 0x80000000u) | 0x7f800000u;
                if ((i & 15) == 4) x &= 0x807fffffu; if ((i & 15) == 5) y &= 0x807fffffu; break;
        case 1: x = norm(-40, 40); y = norm(-40, 40); break;
        case 2: x = norm(0, 0); y = norm(0, 0); break;
        case 3: { y = norm(-20, 20); std::memcpy(&fy_, &y, 4); fx_ = fy_ * (float)(1 + U() % 4096); std::memcpy(&x, &fx_, 4); break; }
        case 4: fx_ = (float)((int)(U() % 2001) - 1000); fy_ = (float)((int)(U() % 2000) + 1) * ((U() & 1) ? 1.f : -1.f); std::memcpy(&x, &fx_, 4); std::memcpy(&y, &fy_, 4); break;
        case 5: { y = norm(-10, 10); std::memcpy(&fy_, &y, 4); uint32_t k = 0x3f800000u + (uint32_t)((int)(U() % 9) - 4); float fk; std::memcpy(&fk, &k, 4); fx_ = fy_ * fk * (float)(1 << (U() % 8)); std::memcpy(&x, &fx_, 4); x += (uint32_t)((int)(U() % 5) - 2); break; }
        case 6: x = norm(-110, -20); y = norm(0, 1); break;
        case 7: { fx_ = ((float)(U() >> 8) / 16777216.f - 0.5f) * 80.f; fy_ = 1.f + std::exp(-fx_); std::memcpy(&x, &fx_, 4); std::memcpy(&y, &fy_, 4); break; }
        case 8: { fx_ = ((float)(U() >> 8) / 16777216.f - 0.5f) * 20.f; fy_ = (float)(U() % 100000 + 1) / 127.f / 5000.f; std::memcpy(&x, &fx_, 4); std::memcpy(&y, &fy_, 4); break; }
        case 9: x = norm(-60, 60); y = (U() & 0x80000000u) | ((uint32_t)(127 - 30 + (int)(U() % 61)) << 23); break;
        case 10: x = norm(-126, -100); y = norm(10, 40); break;
        default: x = norm(100, 127); y = norm(-27, 0); break; }
      a[i] = x; b[i] = y; }
    q.memcpy(da, a.data(), n * 4); q.memcpy(db, b.data(), n * 4); q.memset(cnt, 0, 256); q.wait();
    // per work-group of 16 threads: mismatches of v_fdiv vs div_ieee, lanes on the slow path, native error histogram (0, 1, 2, >2 ulp)
    q.parallel_for(sycl::range<1>(n / 16), [=](sycl::id<1> i) SYCL_ESIMD_KERNEL {
      const vf x = flt(block_load<uint32_t, 16>(da + i * 16)), y = flt(block_load<uint32_t, 16>(db + i * 16));
      DivFast df; const vf ref = div_ieee<float, 16>(x, y), fast = dv(df, x, y), nat = x / y;
      const vu ur = bits(ref), uf = bits(fast), un = bits(nat);
      const vm nanr = (ur & 0x7fffffffu) > 0x7f800000u, nanf = (uf & 0x7fffffffu) > 0x7f800000u;
      vu bad = 0u; bad.merge(vu(1u), (ur != uf) & (df.bad == 0u)); vu fl = df.bad; (void)nanf;
      vi d = vi(un) - vi(ur); d.merge(vi(0) - d, d < 0); vu e1 = 0u, e2 = 0u, e3 = 0u; e1.merge(vu(1u), d == 1); e2.merge(vu(1u), d == 2); e3.merge(vu(1u), (d > 2) & !nanr);
      vu out = 0u; uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0;
      unroll<16>([&](auto li) { constexpr int l = decltype(li)::value; s0 += (uint32_t)bad[l]; s1 += (uint32_t)e1[l]; s2 += (uint32_t)e2[l]; s3 += (uint32_t)e3[l]; s4 += (uint32_t)fl[l]; });
      if (s0 | s1 | s2 | s3 | s4) { out.template select<1, 1>(4) = s4; out.template select<1, 1>(0) = s0; out.template select<1, 1>(1) = s1; out.template select<1, 1>(2) = s2; out.template select<1, 1>(3) = s3;
        atomic_update<atomic_op::add, uint32_t, 16>(cnt, vu(0u, 4u), out, vi(0, 1) < 5); } }).wait();
    uint32_t c[5]; q.memcpy(c, cnt, 20).wait(); total_bad += c[0];
    std::printf("%-66s mismatches %u of %zu, sent to the exact path %.4f %%;  native divide: 1 ulp off %.2f %%, 2 ulp %u, more %u\n", names[cls], c[0], n, 100.0 * c[4] / n, 100.0 * c[1] / n, c[2], c[3]); }
  // timing: 32 dependent-free divisions per thread, 4096 threads, on class "x / (1 + e^-x)"
  for (size_t i = 0; i < n; ++i) { float fx_ = ((float)(U() >> 8) / 16777216.f - 0.5f) * 80.f, fy_ = 1.f + std::exp(-fx_); std::memcpy(&a[i], &fx_, 4); std::memcpy(&b[i], &fy_, 4); }
  q.memcpy(da, a.data(), n * 4); q.memcpy(db, b.data(), n * 4).wait(); float* o = sycl::malloc_device<float>(272 * 512, q);
  auto now = [] { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
  auto run = [&](const char* name, auto&& launch) { for (int it = 0; it < 30; ++it) launch(); q.wait(); double best = 1e18;
    for (int rep = 0; rep < 6; ++rep) { const double t0 = now(); for (int it = 0; it < 300; ++it) launch(); q.wait(); best = std::min(best, (now() - t0) / 300); }
    std::printf("32 divisions per thread, 272 threads, per launch: %-52s %.2f us\n", name, best); };
  const float *fa = (const float*)da, *fb = (const float*)db; float* o_ = o;
#define KERN(NAME, EXPR) run(NAME, [&] { q.parallel_for(sycl::nd_range<1>(272, 34), [=](sycl::nd_item<1> it_) SYCL_ESIMD_KERNEL { const size_t i = it_.get_global_id(0); vf x[32], y[32]; v_load512(x, fa + i * 512); v_load512(y, fb + i * 512); \
    unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; const vf a = x[r], b = y[r]; x[r] = EXPR; }); v_store512(o + i * 512, x); }); });
  namespace ex = sycl::ext::intel::experimental::esimd;
  KERN("no division (a * b)", a * b)
  KERN("native a / b", a / b)
  KERN("div_ieee", (div_ieee<float, 16>(a, b)))
  KERN("DivFast (no branch; bad lanes recorded)", ([&] { DivFast d; const vf r_ = dv(d, a, b); x[(r + 1) & 31] = flt(bits(x[(r + 1) & 31]) | (d.bad & 0u)); return r_; }()))
  KERN("native + 3 fma, no selection", (ex::fma(a / b, b, a) + ex::fma(a, b, a / b) + ex::fma(b, a / b, a)))
  KERN("native, then branch on a mask (never taken)", ([&] { const vf q0 = a / b; if ((bits(q0) == 0x12345u).any()) return a; return q0; }()))
  KERN("native + 4 fma + selection, no branch (unsafe)", ([&] { const vf q0 = a / b; const vu ub = bits(b), uq = bits(q0); const vf nb = flt(ub ^ 0x80000000u), qp = flt(uq + 1u), qm = flt(uq - 1u);
    const vf r0 = ex::fma(q0, nb, a), rp = ex::fma(qp, nb, a), rm = ex::fma(qm, nb, a); vu best = bits(r0) & 0x7fffffffu; const vu ap = bits(rp) & 0x7fffffffu, am = bits(rm) & 0x7fffffffu; vf qq = q0;
    const vm up = ap < best; qq.merge(qp, up); best.merge(ap, up); const vm dn = am < best; qq.merge(qm, dn); best.merge(am, dn); vu st = 0u; st.merge(vu(1u), up); st.merge(vu(0xffffffffu), dn);
    const vf q2 = flt(bits(qq) + st); const vu a2 = bits(ex::fma(q2, nb, a)) & 0x7fffffffu; qq.merge(q2, a2 < best); return qq; }()))
  KERN("a * inv(b) + 2 Newton-free fma steps (unsafe)", ([&] { const vf q0 = a / b; const vf nb = flt(bits(b) ^ 0x80000000u); const vf r0 = ex::fma(q0, nb, a); return q0 + r0 * (vf(1.f) / b); }()))
  KERN("native; exponent tests -> div_ieee (one branch)", ([&] { const vf q0 = a / b; const vu ua = bits(a), ub = bits(b), uq = bits(q0); const vu ea = (ua >> 23) & 0xffu, eb = (ub >> 23) & 0xffu, eq = (uq >> 23) & 0xffu;
    const vm ok = ((eb - 1u) < 253u) & ((((eq - 2u) < 251u) & ((ea - 52u) < 200u)) | ((ua << 1) == 0u)); if (!ok.all()) return div_ieee<float, 16>(a, b); return q0; }()))
  KERN("correction without branches, ONE check at the end -> div_ieee", ([&] { const vf q0 = a / b; const vu ua = bits(a), ub = bits(b), uq = bits(q0); const vu ea = (ua >> 23) & 0xffu, eb = (ub >> 23) & 0xffu, eq = (uq >> 23) & 0xffu;
    const vm ok = ((eb - 1u) < 253u) & ((((eq - 2u) < 251u) & ((ea - 52u) < 200u)) | ((ua << 1) == 0u));
    const vf nb = flt(ub ^ 0x80000000u), qp = flt(uq + 1u), qm = flt(uq - 1u);
    const vf r0 = ex::fma(q0, nb, a), rp = ex::fma(qp, nb, a), rm = ex::fma(qm, nb, a); vu best = bits(r0) & 0x7fffffffu; const vu ap = bits(rp) & 0x7fffffffu, am = bits(rm) & 0x7fffffffu; vf qq = q0;
    const vm up = ap < best; qq.merge(qp, up); best.merge(ap, up); const vm dn = am < best; qq.merge(qm, dn); best.merge(am, dn); vu st = 0u; st.merge(vu(1u), up); st.merge(vu(0xffffffffu), dn);
    const vf q2 = flt(bits(qq) + st); const vu a2 = bits(ex::fma(q2, nb, a)) & 0x7fffffffu;
    if (((a2 < best) | !ok).any()) return div_ieee<float, 16>(a, b); return qq; }()))
  KERN("the same, check by a horizontal max of a lane vector", ([&] { const vf q0 = a / b; const vu ua = bits(a), ub = bits(b), uq = bits(q0); const vu ea = (ua >> 23) & 0xffu, eb = (ub >> 23) & 0xffu, eq = (uq >> 23) & 0xffu;
    const vm ok = ((eb - 1u) < 253u) & ((((eq - 2u) < 251u) & ((ea - 52u) < 200u)) | ((ua << 1) == 0u));
    const vf nb = flt(ub ^ 0x80000000u), qp = flt(uq + 1u), qm = flt(uq - 1u);
    const vf r0 = ex::fma(q0, nb, a), rp = ex::fma(qp, nb, a), rm = ex::fma(qm, nb, a); vu best = bits(r0) & 0x7fffffffu; const vu ap = bits(rp) & 0x7fffffffu, am = bits(rm) & 0x7fffffffu; vf qq = q0;
    const vm up = ap < best; qq.merge(qp, up); best.merge(ap, up); const vm dn = am < best; qq.merge(qm, dn); best.merge(am, dn); vu st = 0u; st.merge(vu(1u), up); st.merge(vu(0xffffffffu), dn);
    const vf q2 = flt(bits(qq) + st); const vu a2 = bits(ex::fma(q2, nb, a)) & 0x7fffffffu;
    vu bad = 0u; bad.merge(vu(1u), (a2 < best) | !ok); if (hmax<uint32_t>(bad) != 0u) return div_ieee<float, 16>(a, b); return qq; }()))
  KERN("native, exponent tests + branch", ([&] { const vf q0 = a / b; const vu ua = bits(a), ub = bits(b), uq = bits(q0); const vu ea = (ua >> 23) & 0xffu, eb = (ub >> 23) & 0xffu, eq = (uq >> 23) & 0xffu;
    const vm ok = ((eb - 1u) < 253u) & ((((eq - 2u) < 251u) & ((ea - 52u) < 200u)) | ((ua << 1) == 0u)); if (!ok.all()) return a; return q0; }()))
  { // the policy used as in the kernels: one DivFast object for many divisions of a thread
    q.memset(cnt, 0, 256).wait();
    q.parallel_for(sycl::nd_range<1>(272, 34), [=](sycl::nd_item<1> it_) SYCL_ESIMD_KERNEL { const size_t i = it_.get_global_id(0); vf x[32], y[32]; v_load512(x, fa + i * 512); v_load512(y, fb + i * 512);
      DivFast d; const vf al = dv(d, vf(hmax<float>(y[0])), vf(127.f)); vu n1 = d.bad;
      unroll<32>([&](auto ri) { constexpr int r = decltype(ri)::value; x[r] = dv(d, x[r], al); }); vu n2 = d.bad;
      unroll<8>([&](auto gi) { v_silu_n<4>(y + 4 * decltype(gi)::value, d); });
      vu o = 0u; uint32_t s1 = 0, s2 = 0, s3 = 0; unroll<16>([&](auto li) { constexpr int l = decltype(li)::value; s1 += (uint32_t)n1[l]; s2 += (uint32_t)n2[l]; s3 += (uint32_t)d.bad[l]; });
      o.template select<1, 1>(0) = s1; o.template select<1, 1>(1) = s2; o.template select<1, 1>(2) = s3; o.template select<1, 1>(3) = failed(d) ? 1u : 0u; o.template select<1, 1>(4) = 1u;
      v_store512(o_ + i * 512, x);
      atomic_update<atomic_op::add, uint32_t, 16>(cnt, vu(0u, 4u), o, vi(0, 1) < 5); }).wait();
    uint32_t c[5]; q.memcpy(c, cnt, 20).wait();
    std::printf("one policy object per thread (max / 127, 32 x / al, 32 silu): marked lanes after the first division %u, after the 32 divisions %u, at the end %u; threads that would repeat %u of %u\n", c[0], c[1], c[2], c[3], c[4]); }
  { // special operands: what the native divide returns next to div_ieee
    const float inf = INFINITY, nan = NAN, den = 1e-42f, tiny = 1.2e-38f, big = 3e38f;
    const float A[16] = {0.f, 1.f, -1.f, 0.f, inf, inf, 1.f, nan, den, 1.f, tiny, big, big, 1e-30f, -0.f, 3.f};
    const float B[16] = {0.f, 0.f, 0.f, 5.f, inf, 2.f, inf, 1.f, 3.f, den, 7.f, 0.5f, big, 1e30f, 4.f, tiny};
    float R[32]; float* dd = sycl::malloc_device<float>(64, q); q.memcpy(dd, A, 64); q.memcpy(dd + 16, B, 64).wait();
    q.parallel_for(sycl::range<1>(1), [=](sycl::id<1>) SYCL_ESIMD_KERNEL { const vf x = block_load<float, 16>(dd), y = block_load<float, 16>(dd + 16);
      DivFast f; const vf e = dv(f, x, y); (void)e; block_store<float, 16>(dd + 32, x / y); block_store<float, 16>(dd + 48, div_ieee<float, 16>(x, y));
      block_store<float, 16>(dd, flt(f.bad)); }).wait();
    q.memcpy(R, dd + 32, 128).wait(); uint32_t mk[16]; q.memcpy(mk, dd, 64).wait();
    std::printf("special operands:        a            b       native     div_ieee   (DivFast marks the lane)\n");
    for (int i = 0; i < 16; ++i) { uint32_t u1, u2; std::memcpy(&u1, &R[i], 4); std::memcpy(&u2, &R[16 + i], 4);
      std::printf("              %12.5g %12.5g %12.5g %12.5g   %s%s\n", A[i], B[i], R[i], R[16 + i], mk[i] ? "marked" : "not marked", (u1 == u2 || (R[i] != R[i] && R[16 + i] != R[16 + i])) ? "" : "   <- native differs"); } }
  std::printf(total_bad ? "RESULT: FAIL\n" : "RESULT: PASS (DivFast is bit-identical to div_ieee on every lane it does not mark)\n"); return total_bad != 0;
}
