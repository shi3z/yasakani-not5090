// ESIMD helpers of src/fast.hpp against the scalar helpers of src/kern.hpp run on the host: every result must be bit-identical.
#include "../src/fast.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace yx; using namespace yx::fx;
static uint32_t fb(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
int main() {
  sycl::queue q{sycl::gpu_selector_v}; const size_t n = 1 << 22; std::mt19937_64 g(7);
  std::vector<float> a(n); std::vector<int64_t> ia(n);
  for (size_t i = 0; i < n; ++i) {
    const int kind = i % 4; uint32_t u = (uint32_t)g();
    if (kind == 0) std::memcpy(&a[i], &u, 4);                                                    // any bit pattern
    else if (kind == 1) a[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 16) - 8) * ((g() & 1) ? 1 : -1);
    else if (kind == 2) a[i] = std::ldexp((float)(g() >> 40) / 16777216.f + 0.5f, (int)(g() % 60) - 40) * ((g() & 1) ? 1 : -1);
    else a[i] = ((float)(g() >> 40) / 16777216.f - 0.5f) * 240.f;
    const int sh = 11 + (int)(g() % 53); ia[i] = (int64_t)(g() >> sh) * ((g() & 1) ? 1 : -1);    // |v| < 2^53
    if (i % 97 == 0) ia[i] = ((int64_t)1 << (24 + i % 29)) + (int64_t)(g() % 5) - 2;             // near ties
  }
  const int NF = 10;
  float *da = sycl::malloc_device<float>(n, q), *dr = sycl::malloc_device<float>(n * NF, q); int64_t* di = sycl::malloc_device<int64_t>(n, q);
  q.memcpy(da, a.data(), n * 4); q.memcpy(di, ia.data(), n * 8); q.wait();
  q.parallel_for(sycl::range<1>(n / 16), [=](sycl::id<1> i) SYCL_ESIMD_KERNEL {
    const vf x = block_load<float, 16>(da + i * 16); const vf ax = flt(bits(x) & 0x7fffffffu); float* o = dr + i * 16;
    block_store<float, 16>(o, v_rbf(x)); block_store<float, 16>(o + n, v_rf16(x)); block_store<float, 16>(o + 2 * n, v_expf(x));
    block_store<float, 16>(o + 3 * n, v_silu(x)); block_store<float, 16>(o + 4 * n, v_sigmoid(x)); block_store<float, 16>(o + 5 * n, v_softplus(x));
    block_store<float, 16>(o + 6 * n, v_logf(ax)); block_store<float, 16>(o + 7 * n, v_rsqrt(ax));
    block_store<float, 16>(o + 8 * n, v_i2f(block_load<int64_t, 16>(di + i * 16)));
    block_store<float, 16>(o + 9 * n, flt(v_f2bf(x))); }).wait();
  std::vector<float> r(n * NF); q.memcpy(r.data(), dr, n * NF * 4).wait();
  const char* nm[NF] = {"rbf", "rf16", "expf", "silu", "sigmoid", "softplus", "logf", "rsqrt", "i64->float", "f2bf"}; int fail = 0;
  for (int k = 0; k < NF; ++k) { size_t bad = 0, nan = 0, first = n;
    for (size_t i = 0; i < n; ++i) { const float x = a[i], ax = std::fabs(x); float e;
      switch (k) { case 0: e = rbf(x); break; case 1: e = rf16(x); break; case 2: e = pm_expf(x); break; case 3: e = pm_silu(x); break; case 4: e = pm_sigmoid(x); break;
        case 5: e = pm_softplus(x); break; case 6: e = pm_logf(ax); break; case 7: e = pm_rsqrt(ax); break; case 8: e = (float)ia[i]; break; default: { uint32_t b = f2bf(x); std::memcpy(&e, &b, 4); } }
      if (fb(e) != fb(r[k * n + i])) { if (std::isnan(e) && std::isnan(r[k * n + i])) ++nan; else { ++bad; if (first == n) first = i; if (bad <= 6) std::printf("   %s x = %a (%08x) host %08x dev %08x\n", nm[k], x, fb(x), fb(e), fb(r[k * n + i])); } } }
    std::printf("%-12s mismatches %zu (nan-payload-only %zu) of %zu", nm[k], bad, nan, n);
    if (bad) std::printf("   first: x = %a (%08x) host %08x dev %08x", a[first], fb(a[first]), 0u, fb(r[k * n + first]));
    std::printf("\n"); fail += bad != 0; }
  // block functions: fwht512, tree128, a8_tail, rfac
  const int NB = 4096, MR = 4; std::vector<float> xb((size_t)NB * 512);
  for (size_t i = 0; i < xb.size(); ++i) xb[i] = ((float)(g() >> 40) / 16777216.f - 0.5f) * std::ldexp(1.f, (int)((i / 512) % 40) - 28);
  for (int i = 0; i < 512; ++i) xb[512 * 5 + i] = 0.f;
  float *dx = sycl::malloc_device<float>(xb.size(), q), *dw = sycl::malloc_device<float>(xb.size(), q), *dt = sycl::malloc_device<float>(NB * 2, q), *dal = sycl::malloc_device<float>(NB, q);
  int8_t* dq = sycl::malloc_device<int8_t>(xb.size(), q); q.memcpy(dx, xb.data(), xb.size() * 4).wait();
  q.parallel_for(sycl::range<1>(NB), [=](sycl::id<1> id) SYCL_ESIMD_KERNEL { const int b = id[0]; vf x[32]; v_load512(x, dx + (size_t)b * 512);
    dt[b] = v_tree128(x + 8); dt[NB + b] = v_rfac(dx + (size_t)b * 512 + 7, 1e-6f)[0];
    v_fwht512(x); v_store512(dw + (size_t)b * 512, x); v_load512(x, dx + (size_t)b * 512);
    v_a8_tail(x, dq, dal, MR, b % MR, b / MR, NB / MR); }).wait();
  std::vector<float> w(xb.size()), t(NB * 2), al(NB), al2(NB); std::vector<int8_t> qq(xb.size()), q2(xb.size());
  q.memcpy(w.data(), dw, w.size() * 4); q.memcpy(t.data(), dt, NB * 8); q.memcpy(al.data(), dal, NB * 4); q.memcpy(qq.data(), dq, qq.size()); q.wait();
  size_t bw = 0, bt = 0, br = 0, ba = 0, bq = 0;
  for (int b = 0; b < NB; ++b) { float buf[512]; std::memcpy(buf, &xb[(size_t)b * 512], 2048);
    bt += fb(tree128(buf + 128)) != fb(t[b]); float S[10]; for (int i = 0; i < 10; ++i) S[i] = std::fabs(buf[7 + i]);
    float sv[16]; std::memcpy(sv, buf + 7, 64); br += fb(rfac(sv, 1e-6f)) != fb(t[NB + b]) && !(std::isnan(rfac(sv, 1e-6f)) && std::isnan(t[NB + b]));
    fwht512(buf); for (int i = 0; i < 512; ++i) bw += fb(buf[i]) != fb(w[(size_t)b * 512 + i]);
    std::memcpy(buf, &xb[(size_t)b * 512], 2048); a8_tail(buf, q2.data(), al2.data(), MR, b % MR, b / MR, NB / MR); }
  for (int b = 0; b < NB; ++b) ba += fb(al[b]) != fb(al2[b]); for (size_t i = 0; i < qq.size(); ++i) bq += qq[i] != q2[i];
  std::printf("fwht512 %zu, tree128 %zu, rfac %zu, a8 alpha %zu, a8 qx %zu mismatches\n", bw, bt, br, ba, bq); fail += (bw || bt || br || ba || bq);
  std::printf(fail ? "FAIL\n" : "ALL EXACT\n"); return fail;
}
