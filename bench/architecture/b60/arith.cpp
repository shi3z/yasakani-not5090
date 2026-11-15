// SPDX-License-Identifier: Apache-2.0
// Scalar / vector arithmetic on the B60: latency (one dependent chain) and throughput (8 independent chains) per operation.
//  kind esimd : explicit simd<T,16> per hardware thread.     kind scalar: plain SYCL, one work-item per lane (the compiler chooses the SIMD width).
//  Every loop trip applies the operation 4 times to each chain.  Integer chains use (x + b) ^ a and (x * a) ^ b so that the
//  compiler cannot fold the chain; float chains are not reassociated (no fast-math).
//  "wide" section: the engine's exact accumulation  acc64 += (int64)d32 * (int64)A32  and alternatives.
// usage: arith     env ARCH_ONLY=esimd,scalar,wide   ARCH_COMPILE_ONLY=1
#include "common.hpp"
using namespace sycl;
using namespace sycl::ext::intel::esimd;
using ab::unroll;

template <class T> inline T vsqrt(T x) { return sycl::sqrt(x); }
template <class T, int N> inline simd<T, N> vsqrt(simd<T, N> x) { return sycl::ext::intel::esimd::sqrt(x); }
struct MulAdd { template <class V> static inline V f(V x, V a, V b) { return x * a + b; } };
struct Add { template <class V> static inline V f(V x, V a, V b) { return x + b; } };
struct Mul { template <class V> static inline V f(V x, V a, V b) { return x * a; } };
struct AddXor { template <class V> static inline V f(V x, V a, V b) { return (x + b) ^ a; } };
struct MulXor { template <class V> static inline V f(V x, V a, V b) { return (x * a) ^ b; } };
struct Div { template <class V> static inline V f(V x, V a, V b) { return a / x; } };
struct DivIeee { template <class V> static inline V f(V x, V a, V b) { return div_ieee(a, x); } };
struct Sqrt { template <class V> static inline V f(V x, V a, V b) { return vsqrt(x); } };
struct SqrtIeee { template <class V> static inline V f(V x, V a, V b) { return sqrt_ieee(x); } };

template <class T, class OP, int NCH>
event k_es(queue& q, float* out, int threads, int iters, T a, T b, T x0) {
  return q.parallel_for(range<1>(threads), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<T, 16> x[NCH]; const simd<T, 16> va(a), vb(b);
    unroll<NCH>([&](auto c) { x[decltype(c)::value] = simd<T, 16>(x0); });
    for (int it = 0; it < iters; ++it) unroll<4>([&](auto) { unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; x[k] = OP::f(x[k], va, vb); }); });
    simd<float, 16> s = 0;
    unroll<NCH>([&](auto c) { s += simd<float, 16>(x[decltype(c)::value]); });
    block_store<float, 16>(out + i[0] * 16, s); });
}
template <class T, class OP, int NCH>
event k_sc(queue& q, float* out, int items, int iters, T a, T b, T x0) {
  return q.parallel_for(range<1>(items), [=](id<1> i) {
    T x[NCH];
    for (int k = 0; k < NCH; ++k) x[k] = x0;
    for (int it = 0; it < iters; ++it) for (int r = 0; r < 4; ++r) for (int k = 0; k < NCH; ++k) x[k] = OP::f(x[k], a, b);
    float s = 0; for (int k = 0; k < NCH; ++k) s += (float)x[k];
    out[i[0]] = s; });
}
// MODE 0: acc64 += sext(d) * (int64)a32   1: accd += double(d) * double(a32)   2: acc32 += d * a32 (wrapping, baseline)
// 3: acc64 += sext(d * a32) (64-bit add only)
template <int MODE>
event k_wide(queue& q, float* out, int threads, int iters, int a32) {
  constexpr int NCH = 4;
  return q.parallel_for(range<1>(threads), [=](id<1> i) SYCL_ESIMD_KERNEL {
    simd<int, 16> d(1, 3); simd<float, 16> s = 0;
    if constexpr (MODE == 1) { simd<double, 16> acc[NCH]; unroll<NCH>([&](auto c) { acc[decltype(c)::value] = 0; });
      for (int it = 0; it < iters; ++it) { unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; const simd<int, 16> dk = d ^ (k * 0x55);
        acc[k] += simd<double, 16>(dk) * (double)a32; }); d += 1; }
      unroll<NCH>([&](auto c) { s += simd<float, 16>(acc[decltype(c)::value]); }); }
    else if constexpr (MODE == 2) { simd<int, 16> acc[NCH]; unroll<NCH>([&](auto c) { acc[decltype(c)::value] = 0; });
      for (int it = 0; it < iters; ++it) { unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; const simd<int, 16> dk = d ^ (k * 0x55);
        acc[k] += dk * a32; }); d += 1; }
      unroll<NCH>([&](auto c) { s += simd<float, 16>(acc[decltype(c)::value]); }); }
    else { simd<int64_t, 16> acc[NCH]; unroll<NCH>([&](auto c) { acc[decltype(c)::value] = 0; });
      for (int it = 0; it < iters; ++it) { unroll<NCH>([&](auto c) { constexpr int k = decltype(c)::value; const simd<int, 16> dk = d ^ (k * 0x55);
        if constexpr (MODE == 0) { simd<int64_t, 16> w = dk; acc[k] += w * (int64_t)a32; }
        else { simd<int64_t, 16> w = dk * a32; acc[k] += w; } }); d += 1; }
      unroll<NCH>([&](auto c) { s += simd<float, 16>(acc[decltype(c)::value]); }); }
    block_store<float, 16>(out + i[0] * 16, s); });
}

int main() {
  queue q = ab::make_queue();
  if (ab::compile_only(q)) return 0;
  ab::banner(q, "arith");
  const device dev = q.get_device();
  const bool f64 = dev.has(aspect::fp64), f16 = dev.has(aspect::fp16);
  std::printf("# aspect::fp64 %d  aspect::fp16 %d  atomic64 %d\n", (int)f64, (int)f16, (int)dev.has(aspect::atomic64));
  float* out = malloc_device<float>(16 * 65536, q);
  std::printf("kind,type,op,ops_per_trip_per_chain,chains,threads,iters,ms,ns_per_op_in_chain,G_lane_ops_per_s\n");
  auto rep = [&](const char* kind, const char* type, const char* op, int nch, int T, int iters, auto&& launch) {
    const double s = ab::best_time(q, launch, true, 0.03, 4);
    std::printf("%s,%s,%s,4,%d,%d,%d,%.4f,%.3f,%.2f\n", kind, type, op, nch, T, iters, s * 1e3, s * 1e9 / (iters * 4.0), (double)T * 16 * iters * 4 * nch / s / 1e9);
    std::fflush(stdout); };
  auto es = [&]<class T, class OP>(const char* type, const char* op, T a, T b, T x0, int iters) {
    for (int T_ : {160, 5120}) {
      rep("esimd", type, op, 1, T_, iters, [&] { return k_es<T, OP, 1>(q, out, T_, iters, a, b, x0); });
      rep("esimd", type, op, 8, T_, iters / 4, [&] { return k_es<T, OP, 8>(q, out, T_, iters / 4, a, b, x0); }); } };
  auto sc = [&]<class T, class OP>(const char* type, const char* op, T a, T b, T x0, int iters) {
    for (int T_ : {160, 5120}) {
      rep("scalar", type, op, 1, T_, iters, [&] { return k_sc<T, OP, 1>(q, out, T_ * 16, iters, a, b, x0); });
      rep("scalar", type, op, 8, T_, iters / 4, [&] { return k_sc<T, OP, 8>(q, out, T_ * 16, iters / 4, a, b, x0); }); } };
  const int IT = 20000;
  if (ab::want("esimd")) {
    es.template operator()<float, MulAdd>("f32", "mul+add", 0.5f, 1.0f, 1.5f, IT);
    es.template operator()<float, Add>("f32", "add", 0.5f, 1.0f, 1.5f, IT);
    es.template operator()<float, Mul>("f32", "mul", 1.0000001f, 1.0f, 1.5f, IT);
    es.template operator()<float, Div>("f32", "div(operator/)", 3.0f, 1.0f, 1.5f, IT / 4);
    es.template operator()<float, DivIeee>("f32", "div_ieee", 3.0f, 1.0f, 1.5f, IT / 4);
    es.template operator()<float, Sqrt>("f32", "sqrt", 3.0f, 1.0f, 1.5f, IT / 4);
    es.template operator()<float, SqrtIeee>("f32", "sqrt_ieee", 3.0f, 1.0f, 1.5f, IT / 4);
    if (f16) es.template operator()<sycl::half, MulAdd>("f16", "mul+add", sycl::half(0.5f), sycl::half(1.0f), sycl::half(1.5f), IT);
    if (f64) { es.template operator()<double, Add>("f64", "add", 0.5, 1.0, 1.5, IT); es.template operator()<double, Mul>("f64", "mul", 1.0000001, 1.0, 1.5, IT);
               es.template operator()<double, MulAdd>("f64", "mul+add", 0.5, 1.0, 1.5, IT); }
    es.template operator()<int32_t, AddXor>("i32", "add+xor", 0x5bd1e995, 0x1234567, 3, IT);
    es.template operator()<int32_t, MulXor>("i32", "mul+xor", 0x5bd1e995, 0x1234567, 3, IT);
    es.template operator()<int64_t, AddXor>("i64", "add+xor", 0x5bd1e9955bd1e995ll, 0x123456789abll, 3, IT);
    es.template operator()<int64_t, MulXor>("i64", "mul+xor", 0x5bd1e9955bd1e995ll, 0x123456789abll, 3, IT / 4);
  }
  if (ab::want("scalar")) {
    sc.template operator()<float, MulAdd>("f32", "mul+add", 0.5f, 1.0f, 1.5f, IT);
    sc.template operator()<float, Div>("f32", "div", 3.0f, 1.0f, 1.5f, IT / 4);
    sc.template operator()<float, Sqrt>("f32", "sqrt", 3.0f, 1.0f, 1.5f, IT / 4);
    if (f16) sc.template operator()<sycl::half, MulAdd>("f16", "mul+add", sycl::half(0.5f), sycl::half(1.0f), sycl::half(1.5f), IT);
    if (f64) sc.template operator()<double, MulAdd>("f64", "mul+add", 0.5, 1.0, 1.5, IT);
    sc.template operator()<int32_t, AddXor>("i32", "add+xor", 0x5bd1e995, 0x1234567, 3, IT);
    sc.template operator()<int32_t, MulXor>("i32", "mul+xor", 0x5bd1e995, 0x1234567, 3, IT);
    sc.template operator()<int64_t, AddXor>("i64", "add+xor", 0x5bd1e9955bd1e995ll, 0x123456789abll, 3, IT);
    sc.template operator()<int64_t, MulXor>("i64", "mul+xor", 0x5bd1e9955bd1e995ll, 0x123456789abll, 3, IT / 4);
  }
  if (ab::want("wide")) {
    std::printf("section,form,threads,iters,ms,ns_per_accumulate_step_in_thread,G_lane_steps_per_s\n");
    const char* nm[] = {"acc64 += sext(d32) * i64(A32)", "accf64 += f64(d32) * f64(A32)", "acc32 += d32 * A32 (wraps)", "acc64 += sext(d32 * A32) (wraps)"};
    auto w = [&]<int MODE>() { if (MODE == 1 && !f64) return;
      for (int T_ : {160, 5120}) { const int iters = 20000; const double s = ab::best_time(q, [&] { return k_wide<MODE>(q, out, T_, iters, 12345677); }, true, 0.03, 4);
        std::printf("wide,%s,%d,%d,%.4f,%.3f,%.2f\n", nm[MODE], T_, iters, s * 1e3, s * 1e9 / (iters * 4.0), (double)T_ * 16 * iters * 4 / s / 1e9); std::fflush(stdout); } };
    w.template operator()<0>(); w.template operator()<1>(); w.template operator()<2>(); w.template operator()<3>();
  }
  free(out, q);
  return 0;
}
