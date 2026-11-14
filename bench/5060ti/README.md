# RTX 5060 Ti (16 GB) records

Measured November 14, 2026 on one GeForce RTX 5060 Ti 16 GB (Intel Core i7-8700, Ubuntu 26.04, driver 595.99.02, CUDA 13.1,
gcc 15.2, `-DCMAKE_CUDA_ARCHITECTURES=120`), speed model, context 65,536 unless noted. The tables made from these records are at
the top of the [README](../../README.md).

| File | What it is |
|---|---|
| `bench_port.py` | the benchmark of the README tables: `yasakani bench` prompts, upstream's three edit / JSONL tasks, the four tasks of docs/TECHNOLOGY.md table 1.3, prefill |
| `longctx.py` | long prompts (8K / 30K / 60K tokens): prefill speed, generation speed after it, peak GPU memory |
| `servertest.py` | streaming requests to `yasakani serve` (speed from the first to the last chunk, tool call) |
| `profsum.py` | prints the per-kernel table of a `YSKN_PROF=1` run: `python profsum.py run.log build/libyskn.so` |
| `results/rtx5060ti_speed.json`, `.txt` | output of `bench_port.py` with the tuned build |
| `results/yasakani_bench_off.json`, `yasakani_bench_think.json` | `yasakani bench --runs 3 --json`, thinking off and on |
| `results/baseline_ctx16k.json` | the four tasks of table 1.3 before the tuning (upstream's settings, context 16,384) |
| `results/sweep_settings.md` | time of one pass for 47 engine settings (the search behind the tuning) |
| `results/longctx.txt`, `results/servertest.txt` | output of the two scripts above |
