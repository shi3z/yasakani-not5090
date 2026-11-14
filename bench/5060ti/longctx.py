#!/usr/bin/env python3
"""Long-prompt check for the 5060 Ti port: prefill speed, generation speed and peak GPU memory at a given prompt length."""
import argparse, subprocess, sys, threading, time
from pathlib import Path
HERE = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser()
ap.add_argument("--model-dir", required=True); ap.add_argument("--dll", default=str(HERE / "build/libyskn.so"))
ap.add_argument("--max-ctx", type=int, default=None); ap.add_argument("--prompt-tokens", default="8000,30000,60000"); ap.add_argument("--tokens", type=int, default=256)
a = ap.parse_args()
from yasakani import Engine, ModelPack
from yasakani.engine import Sampling
from yasakani.textio import ChatFormatter
pack = ModelPack.load(a.model_dir); fmt = ChatFormatter(pack.root)
text = (HERE / "bench/20261101/prompts/long.txt").read_text(encoding="utf-8")
base = fmt.encode(text)
peak = [0]; stop = [False]
def watch():
    while not stop[0]:
        try: peak[0] = max(peak[0], int(subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], capture_output=True, text=True).stdout.split()[0]))
        except Exception: pass
        time.sleep(0.2)
threading.Thread(target=watch, daemon=True).start()
eng = Engine(pack, max_ctx=a.max_ctx, dll=a.dll)
eng.warmup([fmt.encode("warm-up"), fmt.prompt([{"role": "user", "content": "warm-up"}])[0]], n_gen=32)
print("context %d tokens%s; the source text is %d tokens" % (eng.max_ctx, " (auto)" if eng.auto_ctx else "", len(base)), flush=True)
print("%10s %12s %12s %10s %9s %9s  %s" % ("prompt", "prefill s", "prefill t/s", "gen t/s", "ms/pass", "tok/pass", "peak MiB"))
for n in [int(x) for x in a.prompt_tokens.split(",")]:
    reps = n // len(base) + 1
    body = "\n\n".join("[copy %d]\n%s" % (i + 1, text) for i in range(reps))
    ids = fmt.prompt([{"role": "user", "content": body + "\n\nSummarize the text above in about 200 words."}], enable_thinking=False)[0]
    # cut the document part so that the prompt has n tokens (keep the tail with the question and the chat markers)
    if len(ids) > n: ids = list(ids[:n - 64]) + list(ids[-64:])
    eng.reset(); eng.set_sampling(Sampling(0.0, 0.95, 20, 0.0, 0))
    r = eng.generate(ids, a.tokens)
    k = len(r.ids); p = max(1, r.passes)
    print("%10d %12.2f %12.0f %10.1f %9.2f %9.2f  %d" % (len(ids), r.prefill_ms / 1000, len(ids) / (r.prefill_ms / 1000), (k - 1) / (r.gen_ms / 1000), r.gen_ms / p, (k - 1) / p, peak[0]), flush=True)
stop[0] = True; eng.close()
