# Does the native division mode change the QUALITY of the model, measured against text it did not write?
# Teacher-forced on real text (one token per pass): negative log-likelihood of the true next token and top-1 accuracy, in the strict
# mode (0) and in another mode (default 2 = native), on the same tokens.  The per-token difference and its standard error say
# whether the two modes differ beyond noise.  (This compares each mode with the TEXT, not the modes with each other.)
#   python tests/div_nll.py --model-dir models/speed --dll lib/libyskn.so --text <file> [--tokens 1000] [--skip 0] [--mode 2]
import argparse, ctypes as C, json, math
import numpy as np
ap = argparse.ArgumentParser()
ap.add_argument("--model-dir", default="models/speed"); ap.add_argument("--dll", default="lib/libyskn.so"); ap.add_argument("--text", required=True)
ap.add_argument("--tokens", type=int, default=1000); ap.add_argument("--skip", type=int, default=0); ap.add_argument("--mode", type=int, default=2)
ap.add_argument("--max-ctx", type=int, default=8192); ap.add_argument("--out")
a = ap.parse_args()
from yasakani import Engine, ModelPack
from yasakani.textio import ChatFormatter
VOC = 248320
pack = ModelPack.load(a.model_dir); fmt = ChatFormatter(pack.root); eng = Engine(pack, dll=a.dll, max_ctx=a.max_ctx)
lib = C.CDLL(a.dll); lib.yskn_x_set_div.argtypes = [C.c_int]; lib.yskn_x_set_div.restype = C.c_int; f = eng._f; h = eng._e
ids = np.asarray(fmt.encode(open(a.text, encoding="utf-8").read()), np.int32)[a.skip:a.skip + a.tokens + 1]
n = len(ids) - 1; buf = np.zeros(VOC, np.float32); first = C.c_int32(0)
def run(mode):
    lib.yskn_x_set_div(mode); eng.reset(); nll = np.zeros(n); top = np.zeros(n, bool)
    for i in range(n):
        one = ids[i:i + 1].copy(); eng._check(f["yskn_prefill"](h, one.ctypes.data, 1, C.byref(first))); eng._check(f["yskn_logits"](h, buf.ctypes.data))
        x = buf.astype(np.float64); m = x.max(); lse = m + math.log(np.exp(x - m).sum()); nll[i] = lse - x[ids[i + 1]]; top[i] = int(np.argmax(buf)) == int(ids[i + 1])
    return nll, top
n0, t0 = run(0); n1, t1 = run(a.mode); lib.yskn_x_set_div(0); eng.close()
d = n1 - n0; se = d.std(ddof=1) / math.sqrt(n)
print("%d tokens of %s (from token %d)" % (n, a.text.split("/")[-1], a.skip))
print("strict : NLL %.4f nats/token (perplexity %.3f), top-1 %.2f %%" % (n0.mean(), math.exp(n0.mean()), 100 * t0.mean()))
print("mode %d : NLL %.4f nats/token (perplexity %.3f), top-1 %.2f %%" % (a.mode, n1.mean(), math.exp(n1.mean()), 100 * t1.mean()))
print("difference (mode %d - strict): %+.5f nats/token, standard error %.5f (%.1f sigma); per-token |difference|: mean %.4f, max %.3f; top-1 differs at %d positions" %
      (a.mode, d.mean(), se, abs(d.mean()) / se if se > 0 else 0.0, np.abs(d).mean(), np.abs(d).max(), int((t0 != t1).sum())))
if a.out: json.dump({"tokens": n, "nll_strict": float(n0.mean()), "nll_other": float(n1.mean()), "diff": float(d.mean()), "se": float(se), "top1_strict": float(t0.mean()), "top1_other": float(t1.mean())}, open(a.out, "w"))
