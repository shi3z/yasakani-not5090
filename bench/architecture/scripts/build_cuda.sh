#!/bin/bash
# Build the CUDA benchmarks:  scripts/build_cuda.sh [name ...]      (NVCC=<path of nvcc>, default: nvcc from PATH)
# Writes bin/cuda_<name> and bin/cuda_<name>.ptxas.log (registers / stack / spills per kernel as reported by "ptxas -v").
cd "$(dirname "$0")/.." || exit 1
NVCC=${NVCC:-nvcc}
mkdir -p bin
names=("$@"); [ ${#names[@]} -eq 0 ] && names=(gemm_stages lut_variants mma mem arith regs)
for n in "${names[@]}"; do
  echo "== $n"
  "$NVCC" -arch=sm_120 -O3 --fmad=false -std=c++17 -Xptxas -v -o "bin/cuda_$n" "cuda/$n.cu" > "bin/cuda_$n.ptxas.log" 2>&1
  rc=$?
  grep -v -e '^ptxas info' -e '^ *[0-9]* bytes stack frame' "bin/cuda_$n.ptxas.log"
  [ $rc -eq 0 ] || exit 1
done
