# YASAKANI — not only the RTX 5090 (Linux port)

Unofficial **Linux port** of [YASAKANI](https://github.com/yasakani-dev/yasakani)
(Apache-2.0), built from source to run on NVIDIA cards **other than the RTX 5090**.
The upstream release targets Windows 11 and the RTX 5090 (sm_120) only; this fork makes
the same engine build and run on Linux and on other NVIDIA GPUs. Measured so far:

| Card | Arch | VRAM | Models that fit | Self-check |
|---|---|---:|---|---|
| **NVIDIA RTX A6000** | Ampere, sm_86 | 48 GB | quality and speed | 2 of 3 cases |
| **NVIDIA GeForce RTX 5060 Ti** | Blackwell, sm_120 | 16 GB | speed (fastest) model only | **3 of 3 cases** (same bits as the RTX 5090 reference) |
| **Intel Arc Pro B60** (experimental, [SYCL engine](#benchmark-intel-arc-pro-b60-sycl--level-zero-next-to-the-rtx-5060-ti--experimental)) | Xe2 (Battlemage) | 24 GB | quality and speed | speed model: 3 of 3 cases; quality model: 2 of 3 cases |

Other cards need only a different `-DCMAKE_CUDA_ARCHITECTURES` (e.g. 89 for RTX 40,
75 for RTX 20). Not affiliated with or endorsed by the upstream YASAKANI authors. See
[Changes from upstream](#changes-from-upstream).

## Benchmark: RTX 5060 Ti (16 GB) next to the RTX A6000

Same engine, same **speed model** (`qwen38_27b_speed.yskn`, 10.3 GB), same prompts, one
request at a time, greedy, speculative decoding on, `--fmad=false`. The RTX 5090 column is
upstream's published figure for the same model where one exists.

5060 Ti machine: GeForce RTX 5060 Ti 16 GB (stock, 180 W), Intel Core i7-8700, Ubuntu 26.04,
driver 595.99.02, CUDA 13.1 (gcc 15.2), built with `-DCMAKE_CUDA_ARCHITECTURES=120`,
context 65,536 tokens (KV5 cache), measured November 14, 2026. The `yasakani bench` figures
repeat to within 0.3% over 3 runs.

**Decode speed, speed model — tokens/s (the four prompts of `yasakani bench`, 512 tokens)**

| Prompt | RTX 5060 Ti, thinking off | RTX A6000, thinking off | 5060 Ti / A6000 | RTX 5060 Ti, thinking on | Accepted tokens/pass (5060 Ti, off) |
|---|---:|---:|---:|---:|---:|
| explain | 125.6 | 133 | 0.94 | 123.4 | 4.20 |
| code    | 202.9 | 215 | 0.94 | 139.1 | 7.31 |
| story   |  73.5 |  95 | 0.77 |  75.0 | 2.44 |
| proof   | 195.0 | 224 | 0.87 | 156.1 | 7.11 |
| **mean** | **149.2** | **167** | **0.89** | **123.4** | — |

**Edit and JSONL tasks, speed model — tokens/s (thinking off, upstream's own prompts)**

| Prompt (upstream) | RTX 5060 Ti | tokens/pass | RTX A6000 | RTX 5090 (upstream) | 5060 Ti / 5090 |
|---|---:|---:|---:|---:|---:|
| edit_code (Code edit)        | 349 | 14.1 | 413 | 1,222 | 0.29 |
| edit_jsonl (JSONL edit)      | 328 | 13.9 | 388 | 1,156 | 0.28 |
| jsonl_orders_8k (Long JSONL) | 357 | 15.3 | 417 | 1,260 | 0.28 |

On the 5060 Ti the answers of both edit tasks are exactly the expected text.

**The four tasks of upstream's technical notes** ([docs/TECHNOLOGY.md](docs/TECHNOLOGY.md), table 1.3; 768 tokens, context 16,384 on the 5090)

| Task | 5060 Ti: one pass | tokens/pass | RTX 5060 Ti tok/s | RTX 5090 tok/s (upstream) | 5060 Ti / 5090 |
|---|---:|---:|---:|---:|---:|
| no look-ahead (one token per pass) | 26.1 ms | 1.00 | 38.3 | 126 | 0.30 |
| prose (an essay) | 33.3 ms | 2.71 | 81.4 | 279 | 0.29 |
| with thinking (an explanation) | 33.4 ms | 2.91 | 86.9 | 298 | 0.29 |
| code (a web page) | 34.6 ms | 4.31 | 124.6 | about 420 | 0.30 |
| structured (CSV to JSON) | 41.5 ms | 15.04 | 362.2 | 1,264 | 0.29 |

The tokens per pass are the same as upstream's to the last digit, and with look-ahead on
and off the generated tokens are identical. The 5060 Ti runs at 0.28 to 0.30 of the RTX 5090
throughout, a little above the ratio of the memory bandwidths (448 GB/s against 1,792 GB/s = 0.25):
without look-ahead it reads about 10.1 GB per pass in 26.1 ms, which is 86% of what its
memory can deliver (the RTX 5090: 71%). The card is limited by its memory bandwidth, not by its
arithmetic, so there is little left to tune (see below).

**Prefill — prompt processing (speed model)**

| Prompt tokens | RTX 5060 Ti time | RTX 5060 Ti tokens/s | RTX A6000 tokens/s (quality model) |
|---|---:|---:|---:|
| 512    | 0.70 s | 733 | 912 |
| 2,048  | 2.53 s | 809 | 1,122 |
| 8,192  | 10.3 s | 794 | 1,105 |
| 30,000 | 41.6 s | 720 | — |
| 60,000 | 93.4 s | 643 | — |

**Memory on the 16 GB card.** The speed model with its draft model takes about 13.8 GiB before
any context, and the KV5 cache about 21.4 KiB per token. A context of 65,536 tokens uses
15.1 of 15.9 GiB and was tested with a 60,000-token prompt (generation after it: 76 tok/s);
98,304 does not fit. Without `--max-ctx` the package now picks the largest context that fits
the memory that is free (65,536 on an otherwise idle 16 GB card). The quality model
(12.4 GB of weights) does not fit on 16 GB. The model loads in about 4 s.

**What was tuned for the 5060 Ti.** A sweep of the engine's launch settings (47 configurations)
moved the time of one pass by at most 3%; two settings are now chosen automatically on cards with
48 SMs or fewer (twice as many work units per matrix, 3 workers for the output matrix): one pass
with 8 rows went from 34.3 ms to 33.3 ms (prose), with mostly 16 rows from 42.4 ms to 41.5 ms (CSV to JSON),
and the mean of `yasakani bench` from 145.1 to 149.2 tok/s. The output bits do not change (the sums are exact
integers). A per-kernel profile (`YSKN_PROF=1`) shows where the time of an 8-row pass goes: about
65% is the 3-bit weights of the main model, 5% its output matrix, 7% the weights of the draft model, and the rest
the per-row kernels (state, normalisation, attention). The scripts and the raw records are in [`bench/5060ti/`](bench/5060ti/).

**OpenAI-compatible server on the 5060 Ti** (`yasakani serve`, streaming): code with thinking off
143.6 tok/s over 2,048 tokens, a Japanese explanation with thinking on (temperature 1.0)
136.3 tok/s over 4,096 tokens, time to first token 0.12 to 0.19 s; tool calling works.

## Benchmark on an RTX A6000

Measured on this port: one request at a time, quality model (12.4 GB), INT8 KV cache,
speculative decoding on, `--fmad=false`. Machine: RTX A6000 48 GB, AMD Ryzen 9 9950X,
Ubuntu, driver 580.178.04, CUDA 12.8 (gcc 13). Each decode figure is greedy and stable
to within 1% over 3 runs (thinking off) / 2 runs (thinking on).

**Decode speed — tokens/s (greedy)**

| Prompt | Thinking off | Thinking on | Accepted tokens/pass (off) |
|---|---:|---:|---:|
| explain | 117.3 | 103.5 | 4.49 |
| code    | 207.1 | 152.5 | 7.88 |
| story   |  63.9 |  60.7 | 2.46 |
| proof   | 157.5 | 137.0 | 6.10 |
| **mean** | **136.4** | **113.5** | — |

Speed varies 2-3x with content because the draft model's acceptance rate does
(code accepts ~7.9 tokens per forward pass, story ~2.5).

**Edit and JSONL tasks — tokens/s (greedy, thinking off, upstream's own prompts)**

These are upstream's exact `edit_code`, `edit_jsonl` and `jsonl_orders_8k` prompts, so
they compare directly to upstream's Code edit / JSONL edit / Long JSONL columns. Edit and
JSONL generation are the fastest tasks: the output largely copies the input, so the draft
model is accepted ~14-15 tokens per forward pass.

| Prompt (upstream) | A6000 tokens/s | tokens/pass | RTX 5090 (upstream) |
|---|---:|---:|---:|
| edit_code (Code edit)        | 375 | 14.1 | 1,136 |
| edit_jsonl (JSONL edit)      | 380 | 15.1 | 1,151 |
| jsonl_orders_8k (Long JSONL) | 382 | 15.2 | 1,160 |

At ~380 tok/s these run ~2.8x faster than general prose on the A6000, and reach about a
third of the RTX 5090 on the same prompts — in line with the memory-bandwidth gap.

**Translation and structured output — tokens/s (greedy, thinking off, 3 runs)**

| Prompt | tokens/s | Accepted tokens/pass |
|---|---:|---:|
| translation_en_ja | 98.1 | 3.74 |
| translation_ja_en | 147.0 | 5.57 |
| structured_json | 202.9 | 7.76 |
| structured_csv | 91.0 | 3.48 |

These use this port's own representative prompts (short translation and
JSON/CSV generation tasks), not upstream's exact benchmark prompts, so they are
not directly comparable to upstream's Translation/Structured columns; they show
how this card behaves on those task shapes.

**Prefill — prompt processing**

| Prompt tokens | Time | tokens/s |
|---|---:|---:|
| 512   | 0.56 s | 912 |
| 2,048 | 1.83 s | 1,122 |
| 8,192 | 7.42 s | 1,105 |

**Other:** model loads in 3-5 s; about 16 GB of VRAM in use at 8 K context
(11.8 GB weights + 1.4 GB draft head + KV), well within 48 GB.

### Quality model vs speed model (A6000)

Upstream also ships a smaller **speed model** (10.3 GB vs 12.4 GB). It reads less memory
per pass, so it is faster on every task, most on prose. Both measured here the same way
(greedy, thinking off, A6000).

| Task | Quality tok/s | Speed tok/s |
|---|---:|---:|
| explain | 117 | 133 |
| code    | 207 | 215 |
| story   |  64 |  95 |
| proof   | 158 | 224 |
| **general mean** | **136** | **167** |
| edit_code (Code edit)        | 375 | 413 |
| edit_jsonl (JSONL edit)      | 380 | 388 |
| jsonl_orders_8k (Long JSONL) | 382 | 417 |

Download it with `yasakani-dev/Qwen3.8-27B-YASAKANI-speed` and point `--model-dir` at it.

**Versus the RTX 5090.** Upstream's published quality-model figures (thinking off) are
e.g. code 486 and story 179 tokens/s. The A6000 reaches code 207 and story 64 — roughly
1/2.4 to 1/2.8 of the RTX 5090, in line with the memory-bandwidth gap (A6000 0.77 TB/s
vs RTX 5090 ~1.8 TB/s; decode is bandwidth-bound). The prompts differ, so treat this as
an order-of-magnitude comparison, not a controlled one.

## Benchmark: Intel Arc Pro B60 (SYCL / Level Zero) next to the RTX 5060 Ti — experimental

[`intel-arc/`](intel-arc/) holds a separate implementation of the engine in SYCL ESIMD for the
Intel Arc Pro B60 (Xe2, 24 GB). It builds a drop-in `libyskn.so` (`intel-arc/mklib.sh`) that the
Python package loads with `--dll`. The speed model passes `yasakani selfcheck` 3 of 3; the quality
model runs, with one near-tie token different from the reference in the thinking case.

B60 machine: the same computer as the 5060 Ti above (both cards installed; Intel Core i7-8700,
Ubuntu 26.04), Intel oneAPI DPC++ 2026.2 on Level Zero, context 65,536 tokens (KV5 cache), greedy,
speculative decoding on, measured November 14, 2026. The two runs of every prompt gave the same
figure. The model is ready in 7 to 8 s. The 5060 Ti column is the CUDA engine with the speed model
(the quality model with the draft model does not fit 16 GB).

**Decode speed, thinking off — tokens/s (the four prompts of `yasakani bench`, 512 tokens; accepted tokens/pass in parentheses)**

| Prompt | RTX 5060 Ti, speed model | Arc Pro B60, speed model | B60 / 5060 Ti | Arc Pro B60, quality model | B60 / 5060 Ti |
|---|---:|---:|---:|---:|---:|
| explain | 125.5 (4.20) | 72.1 (5.28) | 0.57 | 56.6 (4.27) | 0.45 |
| code    | 202.6 (7.31) | 86.4 (6.56) | 0.43 | 92.1 (7.42) | 0.45 |
| story   |  73.4 (2.44) | 37.1 (2.65) | 0.51 | 34.5 (2.56) | 0.47 |
| proof   | 194.9 (7.11) | 86.4 (6.56) | 0.44 | 78.6 (6.10) | 0.40 |
| **mean** | **149.1** | **70.5** | **0.47** | **65.4** | **0.44** |

**Decode speed, thinking on — tokens/s (accepted tokens/pass in parentheses)**

| Prompt | RTX 5060 Ti, speed model | Arc Pro B60, speed model | B60 / 5060 Ti | Arc Pro B60, quality model | B60 / 5060 Ti |
|---|---:|---:|---:|---:|---:|
| explain | 123.7 (4.13) | 57.6 (4.20) | 0.47 | 61.9 (4.74) | 0.50 |
| code    | 139.4 (4.70) | 64.1 (4.70) | 0.46 | 56.8 (4.49) | 0.41 |
| story   |  75.1 (2.50) | 34.6 (2.49) | 0.46 | 33.2 (2.49) | 0.44 |
| proof   | 156.4 (5.33) | 77.1 (5.75) | 0.49 | 77.0 (6.02) | 0.49 |
| **mean** | **123.6** | **58.3** | **0.47** | **57.2** | **0.46** |

The tokens accepted per pass are about the same on both cards, so the gap is the time of one
pass. The tokens per pass are not identical: the generated tokens were not compared one by one
between the two engines for these prompts, and the B60 arithmetic is not bit-identical to CUDA in
every case (near-ties can fall the other way).

**Long prompts — one document repeated to the given length, then a 256-token summary (speed in tokens/s)**

| Prompt tokens | Card, model | Prompt read | Read speed | Generation | One pass | Tokens/pass |
|---:|---|---:|---:|---:|---:|---:|
| 8,000 | RTX 5060 Ti, speed | 10.1 s | 792 | 99.1 | 34.8 ms | 3.45 |
| 8,000 | Arc Pro B60, speed | 56.4 s | 142 | 26.4 | 114.8 ms | 3.04 |
| 8,000 | Arc Pro B60, quality | 57.9 s | 138 | 29.8 | 118.8 ms | 3.54 |
| 30,000 | RTX 5060 Ti, speed | 41.6 s | 720 | 96.1 | 37.9 ms | 3.64 |
| 30,000 | Arc Pro B60, speed | 400.9 s | 75 | 14.6 | 236.8 ms | 3.45 |
| 30,000 | Arc Pro B60, quality | 406.4 s | 74 | 14.7 | 241.4 ms | 3.54 |
| 60,000 | RTX 5060 Ti, speed | 93.4 s | 643 | 76.5 | 41.7 ms | 3.19 |
| 60,000 | Arc Pro B60, speed | 1,326.5 s | 45 | 9.9 | 413.9 ms | 4.11 |
| 60,000 | Arc Pro B60, quality | 1,337.6 s | 45 | 7.5 | 402.1 ms | 3.00 |

With short contexts the B60 runs at 0.44 to 0.47 of the 5060 Ti. With long contexts it falls far
behind: one pass grows by about 5.8 ms per 1,000 tokens of context on the B60 against about 0.13 ms
on the 5060 Ti, because the attention layers of this implementation are not yet optimised for long
contexts. The 24 GB of the B60 hold the quality model together with the draft model, which 16 GB
do not. Raw records: [intel-arc/bench/](intel-arc/bench/). Details: [intel-arc/README.md](intel-arc/README.md).

**Update (attention rewritten).** The tables above were measured before the attention layers were rewritten to keep decoded
copies of the keys and values in VRAM (4.3 GB at 65,536 tokens) and to use XMX matrix products. One pass with 16 rows at
60,000 tokens of context went from 414 ms to about 126 ms (20,000 tokens: 84 ms; 256 tokens: 73 ms, unchanged). The long-prompt
table will be measured again with this version.

## Build and run on Linux

Needs a CUDA Toolkit (tested with 12.8 for sm_86 and 13.1 for sm_120), CMake >= 3.24, Ninja,
gcc, and an NVIDIA driver.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release      # sm_86 (RTX A6000 / RTX 30) by default
cmake --build build -j            # produces build/libyskn.so
# For another NVIDIA card, set the arch at configure time:
#   -DCMAKE_CUDA_ARCHITECTURES=120   RTX 50 (RTX 5060 Ti: needs CUDA 12.8 or newer)
#   -DCMAKE_CUDA_ARCHITECTURES=89    RTX 40

pip install ./python                                  # the yasakani CLI
huggingface-cli download yasakani-dev/Qwen3.8-27B-YASAKANI-quality --local-dir ./model
yasakani bench --model-dir ./model --dll build/libyskn.so --max-ctx 8192
yasakani chat  --model-dir ./model --dll build/libyskn.so --max-ctx 8192
```

On an **RTX 5060 Ti (16 GB)** use the speed model; the context is chosen automatically:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120
cmake --build build -j
huggingface-cli download yasakani-dev/Qwen3.8-27B-YASAKANI-speed --local-dir ./model-speed
yasakani selfcheck --model-dir ./model-speed --dll build/libyskn.so     # 3 of 3 cases pass
yasakani bench     --model-dir ./model-speed --dll build/libyskn.so
yasakani serve     --model-dir ./model-speed --dll build/libyskn.so
python bench/5060ti/bench_port.py --model-dir ./model-speed            # the tables above
```

The default 262144-token context needs tens of GB for its KV cache. Without `--max-ctx` the
package picks the largest context (a multiple of 16,384 tokens) that fits the GPU memory that
is free at start; pass `--max-ctx` to set it yourself.

## Changes from upstream

All guarded so the Windows / sm_120 build is unaffected.

A6000 port (Linux, sm_86):

- `CMakeLists.txt`: target sm_86 unless `-DCMAKE_CUDA_ARCHITECTURES` is given; give gcc the same `-O3 --fmad=false` as the MSVC path.
- `src/q35_kernels.cu`: guard the `cp.async.bulk.prefetch` (TMA, sm_90+) prefetch hints.
- `src/pdl.cuh`: make `griddepcontrol` (PDL, sm_90+) a no-op below sm_90.
- `src/ysk_compat.cuh` (new), used by 5 files: provide `__nanosleep` via PTX `nanosleep.u32`
  (this toolkit does not declare `__nanosleep` in these translation units).
- `src/engine35.cpp`: `_ftelli64`/`_fseeki64` -> `ftello`/`fseeko` on non-Windows.

RTX 5060 Ti (Linux, sm_120, 16 GB):

- `src/engine35.cpp`: on cards with 48 SMs or fewer, twice as many work units per matrix and 3 workers
  for the output matrix (2 to 3% per pass; same output bits). Larger cards keep the release's values.
- `python/yasakani/engine.py`: without `--max-ctx`, choose the largest context that fits the free GPU memory
  (the release's default of 262,144 tokens cannot be allocated on 16 GB).
- `src/pdl.cuh`, `src/engine.cpp`: `YSKN_PROF=1` times every kernel launch (blocking; for measurements only,
  with the generation setting `use_graph=0`); `bench/5060ti/profsum.py` prints the table.
- `CMakeLists.txt`: `-DCMAKE_CUDA_ARCHITECTURES` given on the command line is now honoured.
- `bench/5060ti/`: the benchmark script of the tables above and its records.

**Correctness note.** On the RTX 5060 Ti (sm_120, CUDA 13.1) `yasakani selfcheck` reproduces all 3 cases.
On the A6000 `yasakani selfcheck` reproduces 2 of its 3 fixed greedy cases exactly
against the upstream reference; the long "thinking" case diverges, consistent with
tensor-core/softmax rounding differing from the reference's sm_120 + CUDA 13.2 build
(short outputs match, long greedy ones drift). Not bit-identical to the RTX 5090 build.

This port keeps the upstream `LICENSE` and `NOTICE`; YASAKANI is Apache-2.0.

---

# YASAKANI

A local LLM engine for **Windows** and **one NVIDIA RTX 5090**, built for one model: **Qwen3.8-27B**. Open source, Apache License 2.0.
日本語は [README.ja.md](README.ja.md)。

> **Version 0.1.1 (November 14, 2026).** New in this version: the server accepts requests with `tools` (tool / function calling), so coding agents and other tool-using apps can connect to it; version 0.1.0 refused them. Engine: [release page](https://github.com/yasakani-dev/yasakani/releases/tag/v0.1.1). Models on Hugging Face: [quality model](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-quality) and [fastest model](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-speed).
>
> **Support the development:** crowdfunding on CAMPFIRE for backers living in Japan, until December 16, 2026 - https://camp-fire.jp/projects/992233/view (outside Japan: see [Support](#support))

## What it is

YASAKANI runs Qwen3.8-27B on your own PC. In a comparison with eight other engines on the same PC (one RTX 5090, Qwen3.8-27B, one request at a
time), its fastest model wrote answers faster than every one of them in all seven task categories, with thinking off and with thinking on. The
tables and what they do and do not show are under [Speed](#speed). It starts an **OpenAI-compatible server** (so chat front-ends and developer
tools can use it), a **terminal chat**, and a **benchmark**. After the download it needs no internet connection.

It was built by one person in Japan. The engine is a purpose-built set of GPU kernels, a file format and a runtime for this one card and
this one model; it is not a general-purpose engine.

## Two models

Both are Qwen3.8-27B, converted to the YASAKANI file format and compressed. You need only one of them.

| | Quality model (recommended) | Fastest model |
|---|---|---|
| Made for | staying close to the original model, at high speed | the highest generation speed and the smallest size |
| Model file | 12.37 GB | 10.31 GB |
| Same most likely next token as the original model | 95.24% | 93.01% |
| KL divergence from the original model (smaller is closer) | 0.0122 | 0.0304 |
| Download | [Hugging Face](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-quality) | [Hugging Face](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-speed) |

The two quality rows are agreement with the original model's next-token prediction on 20 thinking transcripts, not task accuracy; see
[Quality](#quality). The file size is not the GPU memory needed to run the model.

## Speed

Tokens written per second, on one RTX 5090 (stock 600 W, no overclocking), the same PC for every engine, one request at a time. Bigger is faster.

**Thinking off** (greedy; average of 2 runs per task, after leaving out runs that fell into a repetition loop or made a tool call nobody asked for):

| Engine | Code | Story | Translation | Structured | Code edit | JSONL edit | Long JSONL |
|---|---|---|---|---|---|---|---|
| **YASAKANI fastest model** | **529** | **201** | **494** | 909 | **1,222** | **1,156** | **1,260** |
| **YASAKANI quality model** | 486 | 179 | 483 | **930** | 1,136 | 1,151 | 1,160 |
| MegaCapybara 1.39 Tiny | 465 | 164 | 268 | 599 | 441 | 658 | 666 |
| mkyt v0.3.2 | 395 | 181 | 374 | 555 | 423 | 634 | 639 |
| ignis v0.9.6 | 330 | 167 | 298 | 439 | 331 | 510 | 514 |
| NInfer (November 2 build) | 264 | 123 | 257 | 386 | 295 | 443 | 438 |
| vLLM 0.31.0 | 290 | 148 | 261 | 354 | 241 | 426 | 431 |
| SGLang 0.5.22 | 252 | 129 | 246 | 371 | 430 | 466 | 451 |
| llama.cpp b11700 | 242 | 133 | 232 | 337 | 244 | 360 | 362 |
| TensorFold 0.6.6 | 181 | 104 | 172 | 405 | 383 | 408 | 261 |

**Thinking on** (temperature 1.0, top-p 0.95, top-k 20, min-p 0; the fastest run of each task):

| Engine | Code | Story | Translation | Structured | Code edit | JSONL edit | Long JSONL |
|---|---|---|---|---|---|---|---|
| **YASAKANI fastest model** | **337** | **287** | **497** | **536** | 667 | **1,108** | **704** |
| **YASAKANI quality model** | 299 | 242 | 428 | 480 | **743** | 1,053 | 634 |
| MegaCapybara 1.39 Tiny | 290 | 206 | 308 | 429 | 550 | 663 | 516 |
| mkyt v0.3.2 | 244 | 196 | 348 | 384 | 378 | 603 | 454 |
| ignis v0.9.6 | 211 | 130 | 276 | 277 | 278 | 485 | 327 |
| NInfer (November 2 build) | 172 | 140 | 258 | 263 | 263 | 404 | 322 |
| vLLM 0.31.0 | 131 | 112 | 197 | 195 | 187 | 369 | 253 |
| SGLang 0.5.22 | 157 | 124 | 206 | 230 | 257 | 402 | 282 |
| llama.cpp b11700 | 140 | 122 | 184 | 197 | 206 | 349 | 221 |
| TensorFold 0.6.6 | 134 | 79 | 167 | 183 | 439 | 360 | 268 |

What these tables are, and are not:

* "Speed" is the speed of **writing the answer**: (output tokens - 1) / (time from the first to the last content chunk). The time to read the
  prompt is not included.
* The tasks are 12 published prompts (code, story, translation and structured output, 3 each) and 3 tasks of our own (code edit, JSONL edit,
  long JSONL). The output limit is 4,096 or 8,192 tokens.
* Thinking off: the average leaves out runs with a repetition loop or an unrequested tool call, by one rule applied to every engine. This can
  change how many tasks a category holds. For the fastest model's "Code" it matters: 900 tok/s before the exclusion, 529 after; we do not use
  900 as a figure for practical code generation. The table before the exclusion is in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).
* **On November 14, 2026 the rule for repetition was tightened and the table was made again.** The first rule missed some answers that
  repeat whole paragraphs. The fastest model's "Story" value of 285 included such runs (the English mystery story, 454 tok/s). With the new
  rule (for story and translation: a run is left out when lines that occur more than once make up 20% or more of its lines), applied to
  every engine alike, only the "Story" column changes: fastest model 285 -> 201, mkyt 204 -> 181, vLLM 150 -> 148, llama.cpp 139 -> 133.
  The table made again, and the 12 prompts one by one, are in [tools/speed/](tools/speed/).
* The large gaps are in the 3 tasks of our own (1.8 to 2.8 times the next engine). These tasks mostly copy their input, the kind of
  output that speculative decoding predicts best. In the four categories of the 12 published prompts the fastest model is 1.1 to 1.5
  times the next engine. For 3 of the 12 prompts (CUDA code, TypeScript code, the English mystery story) the fastest model's answer was
  a repetition loop or an unrequested tool call, so they are not compared. The quality model's "Story" value is about the same as the
  next engine's (179 against 181).
* Thinking on: for each task the fastest run is taken, then the category is averaged. YASAKANI chose from 5 runs, the other engines from 2,
  so the number of tries is not equal; the table of plain averages is in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md). Many runs ended at the
  output limit, so this is not a comparison of time-to-answer or of accuracy.
* Every engine ran its own weights, quantization, key-value cache format and speed-up settings; this is not a comparison with all settings
  made equal. For an engine measured with several settings, the fastest setting of each category is used.
* YASAKANI's two models were measured on November 8, 2026 with the `yskn.dll` of this release. MegaCapybara with thinking off was measured on
  November 3; its thinking-on runs and the other engines on November 6. This is not a ranking of the latest version of every program, or of
  every use.

Everything needed to check or repeat the comparison (launch settings, prompts, the measurement scripts and the raw records) is in
[`bench/`](bench/). `yasakani bench` measures your own PC.
If you measure with a script of your own, take the token count from the server's `usage.completion_tokens` (in a stream, send `stream_options`
with `{"include_usage": true}`). A stream carries one chunk per token, but where one character is split over several tokens its text arrives
with the last of them, so the number of chunks is slightly smaller than the number of tokens.

### One edit, start to finish

A screen recording without cuts shows the fastest model rewriting a Python file of about 20,000 characters: 5.56 s from sending the request to
the last output (4,712 tokens, 1,102 tok/s while writing, measured while the screen was being recorded). The generated file gave the same
results as the original on 17,701 inputs. The recording, logs, the generated code and the checks are in the release's evidence archive;
see [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

## Quality

The models are compressed (the original BF16 weights are 53.8 GB), so they are **not identical** to the original Qwen3.8-27B.

How close the next-token predictions stay to the original BF16 model was measured on 20 thinking transcripts from our own use of the model,
none of them used when the files were made (20,460 positions, FP16 key-value cache, November 8, 2026). Thirteen configurations of different
engines and compressed files were measured on the same texts:

* The **quality model** has the smallest KL divergence (0.0122) and the highest top-1 agreement (95.24%) of the thirteen.
* Against the GSQ-RCO files of similar size run by llama.cpp: 12 GB class, 95.24% against 93.64% (IQ3_S); 10 GB class, the fastest model's
  93.01% against 91.22% (IQ3_XXS). The KL divergence is about 47% and 41% smaller.

Agreement is "chose the same most likely next token as the original model". It is **not** the rate of correct answers on tasks, and the
result covers thinking text only: on general web text an additional evaluation showed a KL divergence 3-10% larger for the quality model
than for our earlier conversion of the same size. The full table, including the configurations for which only a lower bound of the KL
divergence could be measured, is in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

**Checking it yourself.** The numbers above were measured with a research build of the engine and an FP16 key-value cache. The released
`yskn.dll` works with its 8-bit cache only. Measured again with the released engine as the package starts it (KV5 cache, November 14,
2026), the quality model gives a KL divergence of 0.0126 and a top-1 agreement of 95.35%, the fastest model 0.0309 and 92.98%. The
20 texts, a program that makes the reference from the original model with Hugging Face transformers, and the program that measures a
released model are in [tools/quality/](tools/quality/). The file format is specified in [docs/FORMAT.md](docs/FORMAT.md), and
[tools/yskn_decode.py](tools/yskn_decode.py) reads a model file without the engine and compares the stored weights with the original
ones. The program that made the model files from the original weights is in [tools/convert/](tools/convert/). Run again on the PC
that made them, with only the output matrix taken from the released files, it gave the released files byte for byte
([its README](tools/convert/README.md)).

Speculative decoding only keeps tokens that the main model agrees with, so it is built not to change what is written. Whether a repeated
request gives exactly the same tokens depended, in our checks of November 10, 2026, on whether other programs were using the GPU at the same
time. With the GPU otherwise idle, all 240 repeated runs were identical. While other programs were using the GPU (the desktop was drawing),
a run now and then had the same tokens as the others up to one position and continued differently from there:
1 of 190 runs with the default settings and 21 of 432 with speculative decoding switched off (256 tokens each). The cause has not been found
yet, so this release does not promise bit-identical text from run to run. The details are in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md).

## Long contexts

The context length goes up to 262,144 tokens. The key-value cache is stored in YASAKANI's own compressed format, **KV5**. With the fastest
model and the full 262,144-token context reserved, the engine takes 16.78 GiB with KV5 against 27.13 GiB with an FP16 cache (about 38% less
for the whole engine, about 66% less per cached token). On fixed texts of 2K to 128K tokens, the KL divergence with KV5 was at most 0.00052
higher on average than with FP16. The 262,144 figure is what the engine reserves at start; quality was checked up to 128K, and this is not a
statement that the model runs on a 16 GB GPU.

Together with the helper model that speeds up generation, the GPU memory in use at the full 262,144-token context was about 19.1 GiB for the
fastest model and about 21.2 GiB for the quality model (about 14.3 GiB and 16.4 GiB with `--max-ctx 32768`; November 11, 2026, the difference
of the nvidia-smi values before and after loading).

## What you need

| | |
|---|---|
| GPU | NVIDIA **GeForce RTX 5090** (32 GB). The kernels are built for compute capability 12.0, which the whole RTX 50 series shares, but only the RTX 5090 has been tested, and the quickstart stops on any other card |
| OS | **Windows 11**, 64-bit (the only version tested) |
| Driver | a current NVIDIA driver (tested with 616.92) |
| Software | Microsoft Visual C++ 2015-2022 x64 runtime, Python 3.10 or newer |
| Disk / RAM | 20 GB free (one model is a download of about 13 to 15 GB); 32 GB of RAM recommended (tested on a PC with 96 GB) |

Not supported in this release: Linux, macOS, AMD GPUs, other NVIDIA GPUs (the RTX 4090, 3090 and the 16 GB cards have not been tested with
this release), other models. Support for more GPUs is the purpose of the crowdfunding campaign; see [Support](#support).

Close other programs that use a lot of GPU memory (games, other LLM servers) before you start. When the memory runs out there is no error:
generation just becomes several times slower (a warning is printed at start in that case).

## Quick start

**The quickest way:** unpack `yasakani-quickstart-0.1.1.zip` from the [release page](https://github.com/yasakani-dev/yasakani/releases/tag/v0.1.1) to a plain path such as
`C:\yasakani` and double-click `START.cmd`. It downloads the engine and one model and runs the checks; then you can measure the speed,
compare look-ahead on and off (the numbers of the technical notes, on your own PC), chat in your browser, or start the server. You only answer its questions with a number (Python 3.10 or newer is needed). What it does is in
[quickstart/](quickstart/). To install by hand:

1. Download the engine zip (`yskn.dll` and the Python package) from the [release page](https://github.com/yasakani-dev/yasakani/releases/tag/v0.1.1) and one model folder from
   Hugging Face ([quality](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-quality) or [fastest](https://huggingface.co/yasakani-dev/Qwen3.8-27B-YASAKANI-speed)). Compare the SHA-256 checksums with the ones on the release page.

```bat
hf download yasakani-dev/Qwen3.8-27B-YASAKANI-quality --local-dir D:\models\qwen38-quality
```

2. Install the Python package and check the computer, the files and the installation:

```bat
pip install .
yasakani info      --model-dir D:\models\qwen38-quality --dll D:\yasakani\yskn.dll
yasakani verify    --model-dir D:\models\qwen38-quality
yasakani selfcheck --model-dir D:\models\qwen38-quality --dll D:\yasakani\yskn.dll
```

3. Chat in the terminal, or start the server and point any OpenAI-compatible client at `http://127.0.0.1:8765/v1`:

```bat
yasakani chat  --model-dir D:\models\qwen38-quality --dll D:\yasakani\yskn.dll
yasakani serve --model-dir D:\models\qwen38-quality --dll D:\yasakani\yskn.dll
```

**Good to know before the first request**

* By default the model thinks before it answers, at its longest thinking setting. On requests that ask for a certain number of characters, or
  for a whole program in one go, the thinking alone can take thousands to tens of thousands of tokens, and no answer appears during that time.
  To shorten it: `/effort medium` or `/effort low` in `yasakani chat`, `/think off` to answer without thinking; through the API,
  `reasoning_effort` (`medium`, `low`) or `{"enable_thinking": false}` in `chat_template_kwargs`.
* The output limit (max tokens) includes the thinking. If your client's limit is small, the request ends in the middle of the thinking and the
  answer is empty. Raise the limit when the model is to think (the server's default is 32,768); for requests where even that is not enough, use
  a shorter thinking setting.
* For a long program written in one go, we suggest `medium`. In a small number of trials, `medium` most often gave code that runs as it is;
  the default (longest thinking) can think for minutes and then reach the limit, and thinking off or `low` gave more code that does not run.
* With thinking off, we use temperature 0.7, top-p 0.8 and top-k 20 in development. Temperature 0 falls into repetition more easily on long prose.

The details (the model folder, supported request fields, the loop guard, the self-check) are in [python/README.md](python/README.md).
To build `yskn.dll` yourself, see [BUILDING.md](BUILDING.md); common questions are in [docs/FAQ.md](docs/FAQ.md).

## Use it as an agent

`yasakani-harness-0.1.0.zip` on the [release page](https://github.com/yasakani-dev/yasakani/releases/tag/v0.1.1) opens an agent screen in your browser, run by the model
on your PC: the model reads and writes files in a work folder and runs commands (you choose on the screen whether it may change files).
Run the quickstart above first; then unpack the harness zip into the quickstart folder or next to it and double-click `START_HARNESS.cmd`.

* The screen is [DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness) (open-source software by DeepSeek, MIT license), used
  unchanged. It is installed from npm on first use (about 730 MB; Node.js is fetched too if the PC has none). YASAKANI adds a settings file
  and a Japanese language pack. YASAKANI is not affiliated with DeepSeek, and this combination is not an official product.
* The harness zip is the one of 0.1.0, unchanged. It starts the server with the copy of the Python package that comes with it (0.2.0.dev0);
  the tool-calling code of that copy is the code that is in the standard package since 0.1.1.
* A new session starts with the model's longest thinking setting. One reply may take up to 98,304 tokens; a thinking longer than 49,152
  tokens is closed there and the model goes on to the answer. With both models installed, the screen can switch between them.
* Everything is kept inside the harness folder. A DeepSeek Harness you already use is not touched.

What it does, what was checked and what was not: [harness/HARNESS_README.txt](harness/HARNESS_README.txt).

## How it works, briefly

* A file format of its own for the compressed model, and a runtime built around it. Compression and computation are designed together, so
  that the GPU reads less data and does less work.
* GPU kernels written for the RTX 5090.
* **Speculative decoding.** A small helper model proposes several next tokens at once, and the main model checks them in a single pass and
  accepts the ones it agrees with. The helper model is described in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
* A compressed key-value cache (KV5), which is what lets the full 262,144-token context fit next to the model on a 32 GB card.
* One request is generated at a time.

The source for all of this is in this repository, and so is the program that made the two model files from the original weights
([tools/convert/](tools/convert/); it handles Qwen3.8-27B only). The published models work as they are.

**In detail:** [docs/TECHNOLOGY.md](docs/TECHNOLOGY.md) - technical notes by the author: the arithmetic behind the speed (bytes read
against memory bandwidth), the weight format and how the codes are made, computing on the compressed form, look-ahead, KV5, what each
part contributes, and the source files that go with each.

## Limits

* Windows 11 + RTX 5090 + Qwen3.8-27B only. One request at a time. Text only: no images, no `n` above 1, no `logprobs`. Tool / function calling: `tools` with `tool_choice` `auto` or `none` (new in 0.1.1; a forced `tool_choice` is not supported). It is new and has been tested far less than plain chat.
* The release files are not code-signed, so Windows may warn about them (see [SECURITY.md](SECURITY.md)).
* The answers are not identical to the original model's (see Quality).

## Support

Development is funded by a crowdfunding campaign on CAMPFIRE: https://camp-fire.jp/projects/992233/view (until December 16, 2026). This
release does not depend on its result. The support goes to test hardware, electricity and development time for more GPUs: other RTX cards
first (24 GB and 16 GB), then AMD and Mac, in the order and under the conditions written on the campaign page.
Supporters who wish to be named are credited in this repository, and in the startup credits from a later version. News and
measurements: https://x.com/yasakani_dev

**Outside Japan:** the CAMPFIRE campaign is only for backers living in Japan (CAMPFIRE's rules do not allow digital rewards, like this
campaign's, for backers abroad). From abroad you can support the development through GitHub Sponsors:
https://github.com/sponsors/yasakani-dev - there are no rewards, and the support goes to the same purpose, test hardware for more GPUs.
Starring this repository, trying the engine and reporting what you measure helps a lot too.

## License

Apache License 2.0 - see [LICENSE](LICENSE), [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The model files are
derived from Qwen3.8-27B (Apache-2.0, Alibaba Cloud) and from a publicly released draft model (Apache-2.0, z-lab); they are not the
original weights.
