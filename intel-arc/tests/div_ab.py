# A/B of the division modes of the non-GEMM kernels: strict (0, div_ieee) against native (2) or exact (1).
#   python tests/div_ab.py --model-dir models/speed --dll lib/libyskn.so [--mode 2] [--tokens 200] [--out work/div_ab.json]
# Per prompt (Japanese, English, code, mathematics; thinking off; one token per pass, so every position has its logits):
#   A  strict run, greedy: tokens, logits of every step, DeltaNet states of three layers at chosen steps
#   B  the other mode fed with the tokens of A (teacher forced): logit error per step (max, RMS), arg-max agreement, swaps of the
#      two best candidates, state error over time
#   C  the other mode generating freely: share of identical tokens, first position that differs, and the text of both runs
import argparse, ctypes as C, json, sys
import numpy as np
ap = argparse.ArgumentParser()
ap.add_argument("--model-dir", default="models/speed"); ap.add_argument("--dll", default="lib/libyskn.so"); ap.add_argument("--out")
ap.add_argument("--mode", type=int, default=2); ap.add_argument("--tokens", type=int, default=200); ap.add_argument("--max-ctx", type=int, default=8192)
a = ap.parse_args()
from yasakani import Engine, ModelPack
from yasakani.textio import ChatFormatter
PROMPTS = [
    ("japanese", "日本の四季それぞれの特徴と、季節ごとの代表的な行事を、小学生にもわかるように説明してください。"),
    ("english", "Explain how a refrigerator works, step by step, for a curious teenager."),
    ("code", "Write a Python function that merges overlapping intervals in a list and returns the merged list. Include a short docstring and two test cases."),
    ("math", "Prove that the sum of the first n odd numbers equals n squared, and then compute the sum of the first 25 odd numbers."),
]
VOC = 248320; NS = 48 * 128 * 128; LAYERS = [0, 30, 62]
pack = ModelPack.load(a.model_dir); fmt = ChatFormatter(pack.root); eng = Engine(pack, dll=a.dll, max_ctx=a.max_ctx)
lib = C.CDLL(a.dll); lib.yskn_x_set_div.argtypes = [C.c_int]; lib.yskn_x_set_div.restype = C.c_int
lib.yskn_x_gdn_state.argtypes = [C.c_void_p, C.c_int, C.c_void_p]; f = eng._f; h = eng._e
try: tok = fmt.tok
except AttributeError: tok = None
def detok(ids):
    for name in ("decode", "detokenize", "text"):
        fn = getattr(fmt, name, None)
        if fn:
            try: return fn(list(ids))
            except Exception: pass
    return None
def run(mode, prompt, n, forced=None, keep=False, snaps=()):
    """one token per pass; returns tokens, logits [n, VOC] (if keep) and states {step: [layers]}"""
    lib.yskn_x_set_div(mode); eng.reset(); ids = np.asarray(prompt, np.int32); first = C.c_int32(0)
    eng._check(f["yskn_prefill"](h, ids.ctypes.data, len(ids), C.byref(first)))
    toks = []; lg = np.zeros((n, VOC), np.float32) if keep else None; buf = np.zeros(VOC, np.float32); st = {}
    stats = []; cur = int(first.value)        # prediction after the prompt
    for i in range(n):
        t_in = cur if forced is None else forced[i]; toks.append(t_in if forced is None else cur)
        one = np.asarray([t_in], np.int32); eng._check(f["yskn_prefill"](h, one.ctypes.data, 1, C.byref(first)))
        eng._check(f["yskn_logits"](h, buf.ctypes.data)); cur = int(first.value)
        if keep: lg[i] = buf
        else: stats.append(buf.copy())
        if (i + 1) in snaps:
            s = np.zeros((len(LAYERS), NS), np.float32)
            for j, l in enumerate(LAYERS): eng._check(lib.yskn_x_gdn_state(h, l, s[j].ctypes.data))
            st[i + 1] = s
    return toks, (lg if keep else stats), st, cur
res = {}; bad_tokens = 0
snaps = tuple(s for s in (1, 2, 4, 8, 16, 32, 64, 128, a.tokens) if s <= a.tokens)
for name, text in PROMPTS:
    ids = fmt.prompt([{"role": "user", "content": text}], enable_thinking=False)[0]
    # A: strict.  toks[i] = token fed at step i (the model's own choice), lg[i] = logits after it
    tA, lgA, stA, _ = run(0, ids, a.tokens, keep=True, snaps=snaps)
    # B: the other mode on the same inputs
    tB, lgB, stB, _ = run(a.mode, ids, a.tokens, forced=tA, snaps=snaps)
    mx = []; rms = []; same_top = 0; swap2 = 0; min_margin = 1e9; worst_ratio = 0.0
    for i in range(a.tokens):
        x, y = lgA[i], lgB[i]; d = np.abs(x.astype(np.float64) - y); mx.append(float(d.max())); rms.append(float(np.sqrt((d * d).mean())))
        o = np.argpartition(x, -2)[-2:]; o = o[np.argsort(-x[o])]; margin = float(x[o[0]] - x[o[1]]); min_margin = min(min_margin, margin)
        same_top += int(np.argmax(y) == o[0]); swap2 += int(y[o[1]] > y[o[0]]); worst_ratio = max(worst_ratio, mx[-1] / margin if margin > 0 else float("inf"))
    serr = {str(k): [float(np.linalg.norm(stA[k][j].astype(np.float64) - stB[k][j]) / max(1e-30, np.linalg.norm(stA[k][j].astype(np.float64)))) for j in range(len(LAYERS))] for k in snaps}
    # C: the other mode generating freely
    tC, _, _, _ = run(a.mode, ids, a.tokens)
    eq = [int(x == y) for x, y in zip(tA, tC)]; first_diff = eq.index(0) if 0 in eq else -1; bad_tokens += a.tokens - sum(eq)
    r = {"tokens": a.tokens, "logit_max_abs_err": max(mx), "logit_max_abs_err_mean_over_steps": float(np.mean(mx)), "logit_rms_err_mean": float(np.mean(rms)), "logit_rms_err_max": max(rms),
         "argmax_same_teacher_forced": same_top, "top1_top2_swaps": swap2, "smallest_top1_top2_margin_strict": min_margin, "largest_err_over_margin": worst_ratio,
         "logit_err_by_step": {str(s): [mx[s - 1], rms[s - 1]] for s in snaps}, "state_rel_err_layers_%s" % "_".join(map(str, LAYERS)): serr,
         "free_run_identical_tokens": sum(eq), "free_run_first_difference": first_diff, "ids_strict": [int(t) for t in tA], "ids_other": [int(t) for t in tC]}
    for key, t in (("text_strict", tA), ("text_other", tC)):
        s = detok(t)
        if s is not None: r[key] = s
    res[name] = r
    print("%-9s logits: max |err| %.3g (mean over steps %.3g), RMS %.3g | arg-max same %d/%d, top-2 swaps %d, smallest margin %.3g, max err/margin %.3g" %
          (name, r["logit_max_abs_err"], r["logit_max_abs_err_mean_over_steps"], r["logit_rms_err_mean"], same_top, a.tokens, swap2, min_margin, worst_ratio))
    print("          state rel. error (layers %s) by step: %s" % (LAYERS, "  ".join("%s: %s" % (k, "/".join("%.1e" % v for v in serr[str(k)])) for k in snaps)))
    print("          free run: %d of %d tokens identical, first difference at %s" % (sum(eq), a.tokens, first_diff if first_diff >= 0 else "none"), flush=True)
lib.yskn_x_set_div(0); eng.close()
if a.out: json.dump(res, open(a.out, "w"), ensure_ascii=False, indent=1)
print("RESULT: mode %d vs strict: %s" % (a.mode, "ALL TOKENS IDENTICAL" if bad_tokens == 0 else "%d tokens differ in the free runs" % bad_tokens))
