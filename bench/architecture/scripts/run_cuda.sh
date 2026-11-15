#!/bin/bash
# Run the CUDA benchmarks and write results/5060ti_<name>.csv:  scripts/run_cuda.sh [name ...]
# Refuses to run (exit 75) while any compute process uses an NVIDIA GPU, unless FORCE=1.
# RESULT_PREFIX (default 5060ti) names the result files.
cd "$(dirname "$0")/.." || exit 1
if [ "${FORCE:-0}" != "1" ]; then
  if ! apps=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader); then
    echo "nvidia-smi failed: cannot tell whether the GPU is free (FORCE=1 overrides)" >&2; exit 75
  fi
  if [ -n "$(echo "$apps" | tr -d '[:space:]')" ]; then
    echo "GPU busy: compute process(es) $(echo $apps) present, not running (FORCE=1 overrides)" >&2; exit 75
  fi
fi
mkdir -p results
names=("$@"); [ ${#names[@]} -eq 0 ] && names=(gemm_stages lut_variants mma mem arith regs)
for n in "${names[@]}"; do
  [ -x "bin/cuda_$n" ] || { echo "bin/cuda_$n missing: run scripts/build_cuda.sh" >&2; exit 1; }
  echo "== $n"
  args=(); [ "$n" = regs ] && args=("bin/cuda_regs.ptxas.log")
  "bin/cuda_$n" "${args[@]}" > "results/${RESULT_PREFIX:-5060ti}_$n.csv" || { echo "cuda_$n failed" >&2; exit 1; }
done
