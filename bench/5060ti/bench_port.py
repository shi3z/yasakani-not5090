#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Benchmark of the Linux port, in the shape of the tables at the top of README.md (in-process, greedy, through the Python API).

    python bench/5060ti/bench_port.py --model-dir ./model --dll build/libyskn.so --json out.json

Sections (choose with --sections):
  general   the four prompts of `yasakani bench` (explain, code, story, proof), 512 tokens, thinking off and on
  own       upstream's three tasks (edit_code, edit_jsonl, jsonl_orders_8k) from bench/20261103/prompts/cases.json, thinking off
  table13   the four tasks of docs/TECHNOLOGY.md table 1.3 (quickstart/lookahead_check.py), look-ahead off and on, 768 tokens
  prefill   prompt processing at 512 / 2,048 / 8,192 tokens (and longer with --prefill-tokens)
Speed is (tokens - 1) / the engine's generation time (first to last generated token); the prompt pass is not included.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def gpu():
    try:
        return subprocess.run(["nvidia-smi", "--query-gpu=name,memory.total,memory.used,driver_version", "--format=csv,noheader"],
                              capture_output=True, text=True, timeout=15).stdout.strip()
    except Exception:  # noqa: BLE001
        return "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--dll", default=str(REPO / "build" / "libyskn.so"))
    ap.add_argument("--max-ctx", type=int, default=None, help="default: what the package chooses for this card")
    ap.add_argument("--sections", default="general,own,table13,prefill")
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--prefill-tokens", default="512,2048,8192")
    ap.add_argument("--env", action="append", default=[], help="YSKN_X=V engine setting (repeatable)")
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    from yasakani import Engine, ModelPack, __version__
    from yasakani import cli as ycli
    from yasakani.engine import Sampling
    from yasakani.runtime import RuntimeProfile
    from yasakani.textio import ChatFormatter

    pack = ModelPack.load(a.model_dir)
    fmt = ChatFormatter(pack.root)
    t0 = time.time()
    eng = Engine(pack, max_ctx=a.max_ctx, dll=a.dll, profile=RuntimeProfile.default(pack, dict(kv.split("=", 1) for kv in a.env)))
    eng.warmup([fmt.encode("warm-up"), fmt.prompt([{"role": "user", "content": "warm-up"}])[0]], n_gen=32)
    res = dict(version=__version__, weights=pack.weights.name, gpu=gpu(), max_ctx=eng.max_ctx, load_s=round(time.time() - t0, 1),
               lib_sha256=hashlib.sha256(Path(a.dll).read_bytes()).hexdigest(), sections={})
    print("# %s | %s | context %d | load %.1f s" % (res["weights"], res["gpu"], eng.max_ctx, res["load_s"]), flush=True)
    greedy = Sampling(0.0, 0.95, 20, 0.0, 0)

    def gen(ids, n, spec=None):
        best = None
        for _ in range(a.runs):
            eng.reset()
            eng.set_sampling(greedy)
            r = eng.generate(ids, n, spec=spec)
            k, p = len(r.ids), max(1, r.passes)
            row = dict(prompt_tokens=len(ids), tokens=k, finish=r.finish, passes=r.passes, prefill_s=r.prefill_ms / 1000, gen_s=r.gen_ms / 1000,
                       ms_per_pass=r.gen_ms / p, tok_per_pass=(k - 1) / p, tok_s=(k - 1) / (r.gen_ms / 1000),
                       sha=hashlib.sha256(str(list(r.ids)).encode()).hexdigest()[:12])
            row["runs_tok_s"] = (best["runs_tok_s"] if best else []) + [round(row["tok_s"], 1)]
            if best is None or row["tok_s"] > best["tok_s"]:
                best = dict(row)
            else:
                best["runs_tok_s"] = row["runs_tok_s"]
        return best

    def show(name, r, extra=""):
        print("%-22s prompt %6d  out %5d  %7.2f ms/pass  %6.2f tok/pass  %8.1f tok/s  %s%s" % (name, r["prompt_tokens"], r["tokens"],
              r["ms_per_pass"], r["tok_per_pass"], r["tok_s"], r["finish"], extra), flush=True)

    want = a.sections.split(",")
    if "general" in want:
        out = res["sections"]["general"] = []
        for think in (False, True):
            print("== general, thinking %s (the prompts of `yasakani bench`, 512 tokens)" % ("on" if think else "off"))
            for name, prompt in ycli.BUILTIN_PROMPTS:
                ids = fmt.prompt([{"role": "user", "content": prompt}], enable_thinking=think)[0]
                r = gen(ids, 512)
                r.update(task=name, thinking=think)
                out.append(r)
                show(name, r)
            m = [x["tok_s"] for x in out if x["thinking"] == think]
            print("mean %.1f tok/s" % (sum(m) / len(m)))
    if "own" in want:
        out = res["sections"]["own"] = []
        print("== upstream's three tasks (bench/20261103/prompts/cases.json), thinking off")
        for c in json.loads((REPO / "bench/20261103/prompts/cases.json").read_text(encoding="utf-8")):
            ids = fmt.prompt(c["messages"], enable_thinking=False)[0]
            if len(ids) + int(c["max_new"]) + 64 > eng.max_ctx:
                print("%-22s skipped: needs a context of %d tokens" % (c["name"], len(ids) + int(c["max_new"]) + 64))
                continue
            eng.reset()
            eng.set_sampling(greedy)
            rr = eng.generate(ids, int(c["max_new"]))
            text = fmt.tok.decode([int(i) for i in rr.ids], skip_special_tokens=True) if hasattr(fmt, "tok") else ""
            r = gen(ids, int(c["max_new"]))
            r.update(task=c["name"], thinking=False)
            extra = ""
            if c.get("expected") is not None and text:
                r["exact"] = text.strip() == c["expected"].strip()
                extra = "  exact=%s" % r["exact"]
            out.append(r)
            show(c["name"], r, extra)
    if "table13" in want:
        spec = importlib.util.spec_from_file_location("lookahead_check", REPO / "quickstart" / "lookahead_check.py")
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        out = res["sections"]["table13"] = []
        print("== docs/TECHNOLOGY.md table 1.3 (768 tokens): look-ahead off / on")
        for name, think, prompt in m.TASKS:
            ids = fmt.prompt([{"role": "user", "content": prompt}], enable_thinking=think)[0]
            for mode, sp in (("off", dict(m.NO_LOOKAHEAD)), ("on", None)):
                r = gen(ids, 768, sp)
                r.update(task=name, lookahead=mode, published_5090=m.PUBLISHED.get(name if mode == "on" else "off"))
                out.append(r)
                show("%s (%s)" % (name, mode), r, "  sha " + r["sha"])
    if "prefill" in want:
        out = res["sections"]["prefill"] = []
        print("== prefill (prompt processing)")
        text = (REPO / "bench/20261101/prompts/long.txt").read_text(encoding="utf-8")
        base = [int(x) for x in fmt.encode(text)]
        for n in [int(x) for x in a.prefill_tokens.split(",")]:
            if n + 80 > eng.max_ctx:
                print("%8d tokens: skipped (context %d)" % (n, eng.max_ctx))
                continue
            ids = (base * (n // len(base) + 1))[:n]
            eng.reset()
            eng.set_sampling(greedy)
            r = eng.generate(ids, 2)
            row = dict(prompt_tokens=n, seconds=r.prefill_ms / 1000, tok_s=n / (r.prefill_ms / 1000))
            out.append(row)
            print("%8d tokens  %7.2f s  %7.0f tok/s" % (n, row["seconds"], row["tok_s"]), flush=True)
    res["gpu_after"] = gpu()
    print("# GPU after the runs:", res["gpu_after"])
    eng.close()
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
