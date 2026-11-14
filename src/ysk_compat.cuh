// SPDX-License-Identifier: Apache-2.0
// Portability shim: some CUDA toolkits do not declare __nanosleep in every translation unit.
// nanosleep.u32 is a PTX instruction available from sm_70; a no-op below that.
#pragma once
static __device__ __forceinline__ void ysk_nanosleep(unsigned ns){
#if defined(__CUDA_ARCH__) && (__CUDA_ARCH__ >= 700)
  asm volatile("nanosleep.u32 %0;" :: "r"(ns) : "memory");
#else
  (void)ns;
#endif
}
#ifndef __nanosleep
#define __nanosleep(ns) ysk_nanosleep((unsigned)(ns))
#endif
