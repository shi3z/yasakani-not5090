#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Run the B60 benchmarks in short segments (each under the GPU lock, see b60run.sh) and write results/b60_<name>.csv.
#   scripts/run_b60.sh [gemm_stages gemm_once lut_variants xmx mem arith regs]
# The GPU frequency reported by the xe driver is sampled while a segment runs (results/b60_freq.txt).
cd "$(dirname "$0")/.." || exit 1
mkdir -p results
export SYCL_CACHE_PERSISTENT=1
names=("$@"); [ ${#names[@]} -eq 0 ] && names=(gemm_stages gemm_once lut_variants xmx mem arith regs)
freqfile=$(ls /sys/class/drm/card*/device/tile0/gt0/freq0/act_freq 2>/dev/null | head -n 1)
seg() {   # seg <outfile> <label> <env assignments...> -- <command...>
  local out=$1 label=$2; shift 2
  local envs=(); while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
  local tmp; tmp=$(mktemp)
  if [ -n "$freqfile" ]; then ( while :; do cat "$freqfile"; sleep 0.25; done > "$tmp.f" ) & local fp=$!; fi
  env "${envs[@]}" scripts/b60run.sh "$@" > "$tmp" 2> "$tmp.err"; local rc=$?
  if [ -n "$freqfile" ]; then kill $fp 2>/dev/null; wait $fp 2>/dev/null
    echo "$out $label: act_freq MHz min/median/max $(sort -n "$tmp.f" | awk '{a[NR]=$1} END {if (NR) print a[1], a[int((NR+1)/2)], a[NR], "(" NR " samples; includes idle waiting)"}')" >> results/b60_freq.txt; fi
  touch "$out"; awk -v f="$out" 'BEGIN { while ((getline l < f) > 0) seen[l] = 1 } !((/^#/ || !/[0-9]/) && seen[$0])' "$tmp" > "$tmp.new"; cat "$tmp.new" >> "$out"; rm -f "$tmp.new"
  [ $rc -ne 0 ] && { echo "segment $label failed (rc $rc)" >&2; cat "$tmp.err" >&2; }
  rm -f "$tmp" "$tmp.f" "$tmp.err"
}
for n in "${names[@]}"; do
  out=results/b60_$n.csv; : > "$out"
  ARCH_COMPILE_ONLY=1 "bin/$n" 2>/dev/null        # compile the kernels (persistent cache) before taking the GPU
  case $n in
    gemm_stages)
      for shape in 34816x5120 5120x17408 20480x17408; do for bits in 6 8; do for part in small planar big; do
        seg "$out" "$shape/$bits/$part" ARCH_BITS=$bits ARCH_ONLY=$part -- bin/gemm_stages $shape
      done; done; done ;;
    gemm_once) for shape in 34816x5120 5120x17408; do seg "$out" "$shape" -- bin/gemm_once $shape; done ;;
    lut_variants) for part in micro kernel; do seg "$out" $part ARCH_ONLY=$part -- bin/lut_variants; done ;;
    xmx) for part in reg gemm; do seg "$out" $part ARCH_ONLY=$part -- bin/xmx; done ;;
    mem) for part in ws threads stride slm data; do seg "$out" $part ARCH_ONLY=$part -- bin/mem; done ;;
    arith) for part in esimd scalar wide; do seg "$out" $part ARCH_ONLY=$part -- bin/arith; done ;;
    regs) for part in live group threads slm launch; do seg "$out" $part ARCH_ONLY=$part -- bin/regs; done ;;
  esac
done
