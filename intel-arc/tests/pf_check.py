# Prefill batch check: for several prompt lengths, the first predicted token, the state hashes and a greedy continuation.
#   YX_PF_BATCH=<b> python tests/pf_check.py --dll lib/libyskn.so --out work/pf_<b>.json ; compare the files of two batch sizes with --cmp A B
import argparse, ctypes as C, json, os, sys
import numpy as np
ap = argparse.ArgumentParser()
ap.add_argument("--model", default="models/speed/qwen38_27b_speed.yskn"); ap.add_argument("--dll", default="lib/libyskn.so")
ap.add_argument("--lens", default="16,17,31,32,63,64,65,127,128,129,511,512,513,1000,3000"); ap.add_argument("--chunks", default="100,28,64,7,300")
ap.add_argument("--gen", type=int, default=24); ap.add_argument("--max-ctx", type=int, default=8192); ap.add_argument("--out"); ap.add_argument("--cmp", nargs=2)
a = ap.parse_args()
if a.cmp:
    A, B = (json.load(open(f)) for f in a.cmp); bad = 0
    for k in A:
        same = A[k] == B.get(k); bad += not same; print("%-14s %s" % (k, "identical" if same else "DIFFERENT"));
        if not same:
            for f in A[k]:
                if A[k][f] != B[k].get(f): print("    field", f)
    print("RESULT:", "ALL IDENTICAL" if not bad else "%d DIFFER" % bad); sys.exit(1 if bad else 0)
L = C.CDLL(os.path.abspath(a.dll)); L.yskn_open.restype = C.c_void_p; L.yskn_open.argtypes = [C.c_char_p, C.c_int, C.c_int, C.c_int]; L.yskn_last_error.restype = C.c_char_p
for f in ("yskn_reset", "yskn_prefill", "yskn_state_hash", "yskn_spec_generate", "yskn_position"): getattr(L, f).argtypes = None
e = L.yskn_open(a.model.encode(), a.max_ctx, 0, 0)
if not e: raise SystemExit(L.yskn_last_error().decode())
e = C.c_void_p(e); rng = np.random.default_rng(1); ids = rng.integers(1000, 200000, size=4096).astype(np.int32); res = {}
def ck(r):
    if r != 0: raise SystemExit(L.yskn_last_error().decode())
def state():
    h = (C.c_uint64 * 4)(); n = C.c_int(0); ck(L.yskn_state_hash(e, h, 4, C.byref(n))); return [int(x) for x in h]
def gen(n):
    out = (C.c_int32 * n)(); st = (C.c_double * 64)(); tm = (C.c_double * 64)(); cfg = (C.c_int32 * 1)(0)
    r = L.yskn_spec_generate(e, None, 0, n, cfg, 0, out, st, tm)
    return [int(x) for x in out] if r >= 0 else ["gen error: " + L.yskn_last_error().decode()]
for n in [int(x) for x in a.lens.split(",")]:
    ck(L.yskn_reset(e)); first = C.c_int32(-1); p = ids[:n].copy(); ck(L.yskn_prefill(e, p.ctypes.data_as(C.c_void_p), n, C.byref(first)))
    res["len %d" % n] = {"first": int(first.value), "state": state()}; print("len", n, "first", first.value, flush=True)
# several chunks in a row (states carried across calls), then the state again
ck(L.yskn_reset(e)); o = 0; firsts = []
for n in [int(x) for x in a.chunks.split(",")]:
    first = C.c_int32(-1); p = ids[o:o + n].copy(); ck(L.yskn_prefill(e, p.ctypes.data_as(C.c_void_p), n, C.byref(first))); firsts.append(int(first.value)); o += n
res["chunks"] = {"first": firsts, "state": state()}; print("chunks", firsts, flush=True)
json.dump(res, open(a.out, "w")); print("wrote", a.out)
