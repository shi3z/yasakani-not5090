# YASAKANI on Intel Arc Pro B60 (SYCL / Level Zero)

An unofficial port of the [YASAKANI](https://github.com/yasakani-dev/yasakani) engine (Qwen3.8-27B, `.yskn` models)
from CUDA to SYCL ESIMD for the Intel Arc Pro B60 (Xe2, 24 GB). It builds a drop-in `libyskn.so`
that the upstream Python package loads with `--dll`.

Status: experimental. The fastest model passes `yasakani selfcheck`; the quality model runs
(one near-tie token differs from the CUDA reference in the thinking self-check).

## Build and run

Needs the oneAPI DPC++ compiler (icpx 2026.2, `source /opt/intel/oneapi/setvars.sh`) and the Level Zero GPU driver.
`third_party/nlohmann` of this repository is used for JSON.

    ./mklib.sh                      # -> lib/libyskn.so
    source /opt/intel/oneapi/setvars.sh; export SYCL_CACHE_PERSISTENT=1
    yasakani selfcheck --model-dir <model folder> --dll lib/libyskn.so --max-ctx 8192
    yasakani bench     --model-dir <model folder> --dll lib/libyskn.so --max-ctx 65536

## Results (B60 vs RTX 5060 Ti 16 GB, same machine)

`yasakani bench`, 512 tokens, greedy, context 65,536, mean of four tasks, tokens per second (November 15, 2026).
The 5060 Ti runs the CUDA engine with the fastest model (the quality model does not fit 16 GB with the draft model).

| | 5060 Ti, fastest | B60, fastest | B60, quality |
|---|---|---|---|
| thinking off | 149.1 | 71.8 | 69.2 |
| thinking on | 123.6 | 63.8 | 62.4 |

Long prompts (generation, tokens per second / prompt read, tokens per second):

| prompt tokens | 5060 Ti, fastest | B60, fastest | B60, quality |
|---|---|---|---|
| 8000 | 99.1 / 792 | 48.8 / 551 | 49.0 / 482 |
| 30000 | 96.1 / 720 | 38.5 / 489 | 38.1 / 434 |
| 60000 | 76.5 / 643 | 34.6 / 423 | 29.5 / 381 |

Prompts are read in batches (256 rows for the fastest model, 128 for the quality model, chosen from the free VRAM at start;
`YX_PF_BATCH` overrides it, `YX_MEM=1` prints the allocations). The engine state after a prompt does not depend on the batch.
`PORT_NOTES.md` has the measurements behind these numbers and the approaches that did not work; raw outputs are in `bench/`.

## Layout

- `src/` the engine (`gemm.hpp` exact integer GEMM with XMX, `engine.hpp` layers, `attn.hpp` attention and the packed KV cache, `draft.hpp` draft model, `api.cpp` C interface)
- `ref/` numpy reference used to check bit-exactness
- `spec/` notes on the upstream arithmetic, written from the CUDA source
- `tests/` test and experiment programs (`./build.sh tests/<name>`)
- `PORT_NOTES.md` findings and measurements made during the port

All paths are relative to `intel-arc/`. The tests and helper scripts are run from this folder and expect the model folders
at `models/speed` and `models/quality` (links are fine) and a scratch folder `work/`; the CUDA build of the repository is
expected at `../build/libyskn.so` where a script compares against it.

Not implemented: side candidates and tree passes for sampling, prompt snapshots, `yskn_decode` / `yskn_nll` / `yskn_state_hash`.

## License

Apache License 2.0, as upstream. See `NOTICE`.
