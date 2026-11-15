# Decode check: token ids, position and state hashes after generation with look-ahead must not depend on the engine build nor on the
# pass policy (rows per pass, deferred state replay).  Writes one JSON per run; compare two with --cmp A B.
#   python tests/dec_check.py --model-dir models/speed --dll lib/libyskn.so --out work/dec_a.json [--tokens 200] [--sample]
import argparse, json, sys
ap = argparse.ArgumentParser()
ap.add_argument("--model-dir", default="models/speed"); ap.add_argument("--dll", default="lib/libyskn.so"); ap.add_argument("--out")
ap.add_argument("--tokens", type=int, default=200); ap.add_argument("--max-ctx", type=int, default=8192); ap.add_argument("--sample", action="store_true")
ap.add_argument("--cmp", nargs=2)
a = ap.parse_args()
if a.cmp:
    A, B = (json.load(open(f)) for f in a.cmp); bad = 0
    for k in A:
        same = A[k] == B.get(k); bad += not same; print("%-22s %s" % (k, "identical" if same else "DIFFERENT"))
        if not same:
            for f in A[k]:
                if A[k][f] != B.get(k, {}).get(f): print("    field", f)
    print("RESULT:", "ALL IDENTICAL" if not bad else "%d DIFFER" % bad); sys.exit(1 if bad else 0)
from yasakani import Engine, ModelPack
from yasakani.engine import Sampling
from yasakani.textio import ChatFormatter
from yasakani.cli import BUILTIN_PROMPTS
pack = ModelPack.load(a.model_dir); fmt = ChatFormatter(pack.root); eng = Engine(pack, dll=a.dll, max_ctx=a.max_ctx); res = {}
modes = [("greedy", Sampling(0.0, 0.95, 20, 0.0, 0))] + ([("sample", Sampling(0.7, 0.95, 20, 0.0, 1234))] if a.sample else [])
for name, text in BUILTIN_PROMPTS:
    ids = fmt.prompt([{"role": "user", "content": text}], enable_thinking=False)[0]
    for mname, smp in modes:
        eng.reset(); eng.set_sampling(smp); r = eng.generate(ids, a.tokens, stop_ids=None)
        out = [int(x) for x in r.ids]; h1 = [int(x) for x in eng.state_hash()]; p1 = eng.position()
        # continue the live state (prefix reuse), then a second hash
        ext = list(ids) + out; r2 = eng.generate(ext, 40, stop_ids=None)
        res[name + " " + mname] = {"ids": out, "pos": p1, "state": h1, "ids2": [int(x) for x in r2.ids], "pos2": eng.position(), "state2": [int(x) for x in eng.state_hash()]}
        print("%-8s %-6s %d tokens, %.2f tok/pass, %.1f tok/s" % (name, mname, len(out), len(out) / max(1, r.passes), len(out) / (r.gen_ms / 1000.0)), flush=True)
eng.close(); json.dump(res, open(a.out, "w")); print("wrote", a.out)
