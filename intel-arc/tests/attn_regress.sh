#!/bin/bash
# Attention regression: device (tests/attn_test, build it first: ./build.sh tests/attn_test) vs reference (tests/attn_cmp.py).
#   tests/attn_regress.sh gen     create the synthetic inputs under /data/yasakani/tmp/attn (needs ref_h_<l>.npy of ref/run.py 64 a)
#   tests/attn_regress.sh quick   the two chain tests through layers 0..3 (needs ref/run.py 4 a run with the current reference)
#   tests/attn_regress.sh         everything (about 4 minutes, almost all of it the numpy reference)
# Every line printed must read "RESULT ALL BIT-EXACT".  YX_SCALAR=1 tests the scalar kernels instead of the ESIMD ones.
cd "$(dirname "$0")/.." || exit 1
source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1
export SYCL_CACHE_PERSISTENT=1 SYCL_CACHE_DIR=/data/yasakani/tmp/sycl_cache
mkdir -p /data/yasakani/tmp/attn
P100="16 7 1 16 3 16 16 2 16 7"
if [ "$1" = gen ]; then
  for L in 43 59 63; do python3 tests/attn_cmp.py gen u$L $L 12; done
  for L in 3 43 63; do python3 tests/attn_cmp.py gen v$L $L 100; done
  python3 tests/attn_cmp.py gen w3 3 27 4085; python3 tests/attn_cmp.py gen w51 51 25 1000
  python3 tests/attn_cmp.py gen w43 43 11 4090; python3 tests/attn_cmp.py gen w59 59 35 997
  exit 0
fi
one() { mode=$1; shift; ./tests/attn_test $mode "$@" > /dev/null || echo "DEVICE RUN FAILED: $mode $*"; python3 tests/attn_cmp.py $mode "$@" | grep -E "DIFFERENT|RESULT"; }
one chain c12 3 0 12                    # 12 prompt rows in one pass, layers 0..3; also against ref_h_3.npy
one chain csp 3 0 5 1 2 4               # the same rows in 4 passes
for f in h kp kps vp vps q16 y y2; do cmp -s /data/yasakani/tmp/attn/c12_$f.bin /data/yasakani/tmp/attn/csp_$f.bin || echo "DIFFERENT: one pass vs split passes, $f"; done
[ "$1" = quick ] && exit 0
one unit u43 43 0 7 1 4                 # INT8 V with a guard row (ordinal 10)
one unit u59 59 0 7 1 4                 # INT8 V, low-rank term (ordinal 14)
one unit u63 63 0 7 1 4                 # INT8 V, 1-2 low-rank terms + mean (ordinal 15)
one unit v3 3 0 $P100                   # 100 rows: tiles and splits with several keys
one unit v43 43 0 $P100
one unit v63 63 0 $P100
one unit w3 3 4085 3 8 16               # random cache prefix; crosses len = 4096 (31 -> 42 splits)
one unit w51 51 1000 16 1 8             # K group with a mean (ordinal 12)
one unit w43 43 4090 5 2 4
one unit w59 59 997 16 16 3
