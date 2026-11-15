#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build the SYCL benchmarks:  scripts/build_b60.sh [name ...]      (needs the oneAPI environment: source <oneapi>/setvars.sh)
cd "$(dirname "$0")/.." || exit 1
mkdir -p bin
names=("$@"); [ ${#names[@]} -eq 0 ] && names=(gemm_stages gemm_once lut_variants xmx mem arith regs)
for n in "${names[@]}"; do
  echo "== $n"
  icpx -fsycl -O2 -std=c++20 -ffp-contract=off -fno-fast-math -fsycl-device-code-split=per_kernel \
    -Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off" -o "bin/$n" "b60/$n.cpp" || exit 1
done
