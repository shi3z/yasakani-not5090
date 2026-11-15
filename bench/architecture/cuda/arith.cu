// SPDX-License-Identifier: Apache-2.0
// Scalar arithmetic on CUDA: latency (one dependent chain) and throughput (8 independent chains) per operation (mirror of b60/arith.cpp).
//  One CUDA thread = one scalar lane (the B60 "esimd" kind has 16 lanes per thread), so the thread counts are the B60 ones x 16.
//  Every loop trip applies the operation 4 times to each chain.  Integer chains use (x + b) ^ a and (x * a) ^ b so that the
//  compiler cannot fold the chain; float chains are not reassociated.  Built with --fmad=false: "mul+add" is two instructions,
//  "fma" is the explicit fused operation.  The chains start from different values so that they cannot be merged.
//  "wide" section: the engine's exact accumulation  acc64 += (int64)d32 * (int64)A32  and alternatives.
// usage: arith     env ARCH_ONLY=ops,wide
#include "common.cuh"
#include <cuda_fp16.h>

// Kernel arguments are passed as (double, long long) pairs and converted on the device.
template <class T> struct Tr;
template <> struct Tr<float> { static __device__ __forceinline__ float mk(double f, long long) { return (float)f; } static __device__ __forceinline__ float tof(float x) { return x; } };
template <> struct Tr<double> { static __device__ __forceinline__ double mk(double f, long long) { return f; } static __device__ __forceinline__ float tof(double x) { return (float)x; } };
template <> struct Tr<__half> { static __device__ __forceinline__ __half mk(double f, long long) { return __float2half((float)f); } static __device__ __forceinline__ float tof(__half x) { return __half2float(x); } };
template <> struct Tr<int32_t> { static __device__ __forceinline__ int32_t mk(double, long long i) { return (int32_t)i; } static __device__ __forceinline__ float tof(int32_t x) { return (float)x; } };
template <> struct Tr<long long> { static __device__ __forceinline__ long long mk(double, long long i) { return i; } static __device__ __forceinline__ float tof(long long x) { return (float)x; } };

__device__ __forceinline__ float fma_of(float x, float a, float b) { return fmaf(x, a, b); }
__device__ __forceinline__ double fma_of(double x, double a, double b) { return fma(x, a, b); }
__device__ __forceinline__ __half fma_of(__half x, __half a, __half b) { return __hfma(x, a, b); }
__device__ __forceinline__ float sqrt_of(float x) { return sqrtf(x); }
__device__ __forceinline__ double sqrt_of(double x) { return sqrt(x); }
struct MulAdd { template <class V> static __device__ __forceinline__ V f(V x, V a, V b) { return x * a + b; } };
struct Fma { template <class V> static __device__ __forceinline__ V f(V x, V a, V b) { return fma_of(x, a, b); } };
struct Add { template <class V> static __device__ __forceinline__ V f(V x, V, V b) { return x + b; } };
struct Mul { template <class V> static __device__ __forceinline__ V f(V x, V a, V) { return x * a; } };
struct AddXor { template <class V> static __device__ __forceinline__ V f(V x, V a, V b) { return (x + b) ^ a; } };
struct MulXor { template <class V> static __device__ __forceinline__ V f(V x, V a, V b) { return (x * a) ^ b; } };
struct Div { template <class V> static __device__ __forceinline__ V f(V x, V a, V) { return a / x; } };
struct Sqrt { template <class V> static __device__ __forceinline__ V f(V x, V, V) { return sqrt_of(x); } };

template <class T, class OP, int NCH>
__global__ void k_ar(float* out, int iters, double af, long long ai, double bf, long long bi, double xf, long long xi) {
  const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
  const T a = Tr<T>::mk(af, ai), b = Tr<T>::mk(bf, bi);
  T x[NCH];
  #pragma unroll
  for (int k = 0; k < NCH; ++k) x[k] = Tr<T>::mk(xf + 0.0625 * k, xi + k);
  for (int it = 0; it < iters; ++it) {
    #pragma unroll
    for (int r = 0; r < 4; ++r) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) x[k] = OP::f(x[k], a, b);
    }
  }
  float s = 0.f;
  #pragma unroll
  for (int k = 0; k < NCH; ++k) s += Tr<T>::tof(x[k]);
  out[gid] = s;
}

__device__ __forceinline__ void inc_s32(int32_t& d) { asm volatile("add.s32 %0, %0, 1;\n" : "+r"(d)); }     // opaque to the optimizer: no closed form for the sums below
// MODE 0: acc64 += sext(d) * (int64)a32   1: accd += double(d) * double(a32)   2: acc32 += d * a32 (wrapping, baseline)
// 3: acc64 += sext(d * a32) (64-bit add only)
template <int MODE>
__global__ void k_wide(float* out, int iters, int a32) {
  constexpr int NCH = 4;
  const uint32_t gid = blockIdx.x * blockDim.x + threadIdx.x;
  int32_t d = 1 + 3 * (int32_t)(gid & 15u); float s = 0.f;
  if constexpr (MODE == 1) { double acc[NCH] = {0, 0, 0, 0}; const double ad = (double)a32;
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) { const int32_t dk = d ^ (k * 0x55); acc[k] += (double)dk * ad; }
      inc_s32(d); }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += (float)acc[k]; }
  else if constexpr (MODE == 2) { uint32_t acc[NCH] = {0, 0, 0, 0};
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) { const int32_t dk = d ^ (k * 0x55); acc[k] += (uint32_t)dk * (uint32_t)a32; }
      inc_s32(d); }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += (float)(int32_t)acc[k]; }
  else { long long acc[NCH] = {0, 0, 0, 0};
    for (int it = 0; it < iters; ++it) {
      #pragma unroll
      for (int k = 0; k < NCH; ++k) { const int32_t dk = d ^ (k * 0x55);
        if constexpr (MODE == 0) acc[k] += (long long)dk * (long long)a32;
        else acc[k] += (long long)(int32_t)((uint32_t)dk * (uint32_t)a32); }
      inc_s32(d); }
    #pragma unroll
    for (int k = 0; k < NCH; ++k) s += (float)acc[k]; }
  out[gid] = s;
}

static float* g_out = nullptr;
static void rep(const char* type, const char* op, int nch, int T, int iters, double s) {
  if (s <= 0) return;
  std::printf("cuda,%s,%s,4,%d,%d,%d,%.4f,%.3f,%.2f,%d\n", type, op, nch, T, iters, s * 1e3, s * 1e9 / (iters * 4.0), (double)T * iters * 4 * nch / s / 1e9, T / 32);
  std::fflush(stdout);
}
template <class T, class OP> static void ops(const char* type, const char* op, double af, long long ai, double bf, long long bi, double xf, long long xi, int iters) {
  for (int T_ : {2560, 81920}) {         // = 160 and 5120 B60 threads of 16 lanes
    rep(type, op, 1, T_, iters, ab::best_time([&] { k_ar<T, OP, 1><<<T_ / 256, 256>>>(g_out, iters, af, ai, bf, bi, xf, xi); }, true, 0.03, 4));
    rep(type, op, 8, T_, iters / 4, ab::best_time([&] { k_ar<T, OP, 8><<<T_ / 256, 256>>>(g_out, iters / 4, af, ai, bf, bi, xf, xi); }, true, 0.03, 4));
  }
}
template <int MODE> static void wide(const char* name) {
  for (int T_ : {2560, 81920}) { const int iters = 200000;
    const double s = ab::best_time([&] { k_wide<MODE><<<T_ / 256, 256>>>(g_out, iters, 12345677); }, true, 0.03, 4);
    if (s > 0) std::printf("wide,%s,%d,%d,%.4f,%.3f,%.2f\n", name, T_, iters, s * 1e3, s * 1e9 / (iters * 4.0), (double)T_ * iters * 4 / s / 1e9);
    std::fflush(stdout); }
}

int main() {
  ab::banner("arith");
  g_out = ab::dalloc<float>(81920);
  const int IT = 200000;
  if (ab::want("ops")) {
    std::printf("kind,type,op,ops_per_trip_per_chain,chains,threads,iters,ms,ns_per_op_in_chain,G_lane_ops_per_s,warps\n");
    std::printf("# CUDA: threads = CUDA threads = scalar lanes; div and sqrt are the correctly rounded IEEE operations\n");
    ops<float, MulAdd>("f32", "mul+add", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<float, Fma>("f32", "fma", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<float, Add>("f32", "add", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<float, Mul>("f32", "mul", 1.0000001, 0, 1.0, 0, 1.5, 0, IT);
    ops<float, Div>("f32", "div(operator/)", 3.0, 0, 1.0, 0, 1.5, 0, IT / 4);
    ops<float, Sqrt>("f32", "sqrt", 3.0, 0, 1.0, 0, 1.5, 0, IT / 4);
    ops<__half, MulAdd>("f16", "mul+add", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<__half, Fma>("f16", "fma", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<double, Add>("f64", "add", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<double, Mul>("f64", "mul", 1.0000001, 0, 1.0, 0, 1.5, 0, IT);
    ops<double, MulAdd>("f64", "mul+add", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<double, Fma>("f64", "fma", 0.5, 0, 1.0, 0, 1.5, 0, IT);
    ops<int32_t, AddXor>("i32", "add+xor", 0, 0x5bd1e995, 0, 0x1234567, 0, 3, IT);
    ops<int32_t, MulXor>("i32", "mul+xor", 0, 0x5bd1e995, 0, 0x1234567, 0, 3, IT);
    ops<long long, AddXor>("i64", "add+xor", 0, 0x5bd1e9955bd1e995ll, 0, 0x123456789abll, 0, 3, IT);
    ops<long long, MulXor>("i64", "mul+xor", 0, 0x5bd1e9955bd1e995ll, 0, 0x123456789abll, 0, 3, IT / 4);
  }
  if (ab::want("wide")) {
    std::printf("section,form,threads,iters,ms,ns_per_accumulate_step_in_thread,G_lane_steps_per_s\n");
    wide<0>("acc64 += sext(d32) * i64(A32)"); wide<1>("accf64 += f64(d32) * f64(A32)"); wide<2>("acc32 += d32 * A32 (wraps)"); wide<3>("acc64 += sext(d32 * A32) (wraps)");
  }
  cudaFree(g_out);
  return 0;
}
