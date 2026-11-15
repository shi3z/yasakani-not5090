# Look-ahead on/off check for the Xe port: the ids must not depend on drafting (drafter, D0/D1) nor on prefix reuse.
#   source /opt/intel/oneapi/setvars.sh; SYCL_CACHE_PERSISTENT=1 YX_DSTAT=1 ~/yasakani-src/b60/.venv/bin/python tests/la_check.py [--tokens 320] [--sample] [--tasks explain,code]
import argparse, sys, time
import numpy as np
from yasakani import Engine, ModelPack
from yasakani.engine import Sampling
from yasakani.textio import ChatFormatter
from yasakani.cli import BUILTIN_PROMPTS

ap = argparse.ArgumentParser()
ap.add_argument("--model-dir", default="/data/yasakani/model-speed")
ap.add_argument("--dll", default="/home/shi3z/yasakani-src/xe/lib/libyskn.so")
ap.add_argument("--tokens", type=int, default=320)
ap.add_argument("--max-ctx", type=int, default=8192)
ap.add_argument("--sample", action="store_true", help="also compare with sampling (T=0.7, seed 1234)")
ap.add_argument("--reuse", action="store_true", help="also check the live-prefix continuation (mode 1)")
ap.add_argument("--tasks", default="explain,code,story,proof")
ap.add_argument("--dump", help="only run the default settings and write the ids to this file (compare a run with YX_NO_LOOKAHEAD=1 against one without)")
a = ap.parse_args()
pack = ModelPack.load(a.model_dir); fmt = ChatFormatter(pack.root)
eng = Engine(pack, dll=a.dll, max_ctx=a.max_ctx)
eng.warmup([fmt.encode("warm-up"), fmt.prompt([{"role": "user", "content": "warm-up"}])[0]], n_gen=32)
NO = dict(bdraft=0, use_d0=0, use_d1=0)
ok = True


def run(ids, spec, smp, reset=True):
    if reset:
        eng.reset()
    eng.set_sampling(smp)
    r = eng.generate(ids, a.tokens, spec=spec, stop_ids=None)
    n = len(r.ids)
    return [int(x) for x in r.ids], r.passes, n / (r.gen_ms / 1000.0), n / max(1, r.passes), r


modes = [("greedy", Sampling(0.0, 0.95, 20, 0.0, 0))] + ([("sample", Sampling(0.7, 0.95, 20, 0.0, 1234))] if a.sample else [])
for name, text in BUILTIN_PROMPTS:
    if name not in a.tasks.split(","):
        continue
    ids = fmt.prompt([{"role": "user", "content": text}], enable_thinking=False)[0]
    if a.dump:
        full, p1, s1, tp1, _ = run(ids, None, modes[0][1])
        open(a.dump, "a").write(name + " " + " ".join(map(str, full)) + "\n")
        print("%-8s n=%d %5.1f tok/s %.2f tok/pass" % (name, len(full), s1, tp1), flush=True)
        continue
    for mname, smp in modes:
        base, p0, s0, tp0, _ = run(ids, NO, smp)
        full, p1, s1, tp1, _ = run(ids, None, smp)
        nod, p2, s2, tp2, _ = run(ids, dict(bdraft=0), smp)
        same = base == full and base == nod
        ok &= same
        print("%-8s %-6s n=%d  off: %5.1f tok/s %.2f tok/pass | drafter: %5.1f tok/s %.2f tok/pass | D0/D1 only: %5.1f tok/s %.2f | ids %s" %
              (name, mname, len(base), s0, tp0, s1, tp1, s2, tp2, "IDENTICAL" if same else "DIFFER at %d" %
               next((i for i, (x, y, z) in enumerate(zip(base, full, nod)) if x != y or x != z), -1)), flush=True)
        if a.reuse:
            # continue the live state: prompt + everything generated except the last id, plus a few more prompt ids
            k = len(full) // 2
            eng.reset(); eng.set_sampling(smp)
            r1 = eng.generate(ids, k, stop_ids=None)
            ext = list(ids) + [int(x) for x in r1.ids]
            r2 = eng.generate(ext, a.tokens - k, stop_ids=None)          # no reset: mode 1
            eng.reset(); eng.set_sampling(smp)
            r3 = eng.generate(ext, a.tokens - k, spec=NO, stop_ids=None)
            same = [int(x) for x in r2.ids] == [int(x) for x in r3.ids]
            ok &= same
            print("   reuse: mode %d cached %d, %.2f tok/pass, ids vs fresh no-look-ahead run: %s" % (r2.snap_mode if hasattr(r2, "snap_mode") else -1,
                  getattr(r2, "cached_tokens", -1), len(r2.ids) / max(1, r2.passes), "IDENTICAL" if same else "DIFFER"), flush=True)
eng.close()
print("RESULT:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
