#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Static code generation of the YASAKANI compressed-GEMM CUDA kernels: objects, PTX and SASS listings.
# Nothing is executed on a GPU; only the compiler and the disassembler run.
#
#   scripts/cuda_codegen.sh <upstream source tree> [output directory]
#
#   $1 / YSKN_SRC : upstream tree (the directory that contains src/, include/ and third_party/)
#   $2            : output directory (default: results/cuda_codegen, relative to the current directory)
#   NVCC          : CUDA compiler             (default: nvcc)
#   CUOBJDUMP     : CUDA object disassembler  (default: cuobjdump)
#   CUDA_ARCH     : target architecture       (default: sm_120)
#   YSKN_UNITS    : translation units         (default: "gemm_compressed lnord pfa q35_g6")
#
# Compiler flags follow the upstream CMakeLists.txt: -O3 --fmad=false, -DYSKN_KVX_MODE=3,
# include directories src, include and third_party.
#
# Output per unit <u>:  <u>.o  <u>.ptx  <u>.sass  (cuobjdump -sass)  <u>.elf.txt  (cuobjdump -elf, holds .nv.info)
set -u
SRC="${1:-${YSKN_SRC:-}}"
OUT="${2:-results/cuda_codegen}"
NVCC="${NVCC:-nvcc}"
CUOBJDUMP="${CUOBJDUMP:-cuobjdump}"
ARCH="${CUDA_ARCH:-sm_120}"
UNITS="${YSKN_UNITS:-gemm_compressed lnord pfa q35_g6}"

if [ -z "$SRC" ] || [ ! -d "$SRC/src" ]; then
  echo "usage: $0 <upstream source tree> [output directory]   (or set YSKN_SRC)" >&2
  exit 2
fi
command -v "$NVCC" >/dev/null 2>&1 || { echo "compiler not found: $NVCC (set NVCC)" >&2; exit 2; }
command -v "$CUOBJDUMP" >/dev/null 2>&1 || { echo "disassembler not found: $CUOBJDUMP (set CUOBJDUMP)" >&2; exit 2; }

mkdir -p "$OUT" || exit 1
FLAGS=(-arch="$ARCH" -O3 --fmad=false -std=c++17 -DYSKN_KVX_MODE=3 "-I$SRC/src" "-I$SRC/include")
[ -d "$SRC/third_party" ] && FLAGS+=("-I$SRC/third_party")

{
  echo "arch:      $ARCH"
  echo "flags:     -arch=$ARCH -O3 --fmad=false -std=c++17 -DYSKN_KVX_MODE=3 -I<src> -I<include> -I<third_party>"
  echo "compiler:  $("$NVCC" --version 2>/dev/null | grep -i -m1 'release')"
  echo "units:     $UNITS"
} > "$OUT/BUILD_INFO.txt"

rc=0
for u in $UNITS; do
  f="$SRC/src/$u.cu"
  if [ ! -f "$f" ]; then echo "missing: src/$u.cu" >&2; rc=1; continue; fi
  echo "== $u"
  "$NVCC" "${FLAGS[@]}" -c "$f" -o "$OUT/$u.o" || { rc=1; continue; }
  "$NVCC" "${FLAGS[@]}" -ptx "$f" -o "$OUT/$u.ptx" || rc=1
  "$CUOBJDUMP" -sass "$OUT/$u.o" > "$OUT/$u.sass" || rc=1
  "$CUOBJDUMP" -elf "$OUT/$u.o" > "$OUT/$u.elf.txt" 2>/dev/null || true
  echo "   functions: $(grep -c '^[[:space:]]*Function : ' "$OUT/$u.sass")   sass lines: $(wc -l < "$OUT/$u.sass")"
done
exit $rc
