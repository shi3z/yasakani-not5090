# YASAKANI → Intel Arc Pro B60 (SYCL + Level Zero) port notes

Goal (user, 2026-11-14): port the whole YASAKANI engine (Qwen3.8-27B, .yskn format) to the B60 with SYCL/Level Zero.
It must be faster than the OpenCL prototype. User rejected OpenCL as the final API.

## Layout
- `../` upstream clone (c0e11b9) + 2-line Linux fix in `src/engine35.cpp` (`ftello/fseeko`).
  CUDA build: `build/libyskn.so` (nvcc 13.1 at /usr/local/cuda-13.1). Works on RTX 5060 Ti: selfcheck PASS,
  bench mean 144.9 tok/s (ctx 8192), ~35 ms per pass. Uses 14.5 GB VRAM.
- Model: `models/speed` (a link to the model folder).
- `b60/` OpenCL probes + venv (`.venv/bin/yasakani` CLI, pyopencl). `probe4.py` = OpenCL XMX kernel.
- `./` SYCL work. `gemm_esimd2.cpp` = current best kernel. `dump.py` writes test tensors to `work/`.
- Compiler: `source /opt/intel/oneapi/setvars.sh; icpx -fsycl -O2 -std=c++20`. Do NOT wrap in `sg render` (drops LD_LIBRARY_PATH;
  user is already in group render). Device: `[level_zero:gpu] Arc Pro B60`.
- Oracle for validation: CUDA engine exports `yskn_set_capture(layers)`, `yskn_capture_rows`, `yskn_logits`, `yskn_hidden_rows`,
  `yskn_verify` (see `include/yskn_abi.h`); `tools/yskn_decode.py` decodes tensors on CPU.
- Haru and XeStrata are stopped (user asked to end Haru). B60 and 5060 Ti are both free.

## Hot kernel findings (compressed GEMM, 3-bit trellis code → int8 → dot)
- B60 read bandwidth ≈ 450 GB/s → floor ≈ 21 ms per pass (25.6 G weights, 9.6 GB of codes).
- The value-table lookup dominates. Table (4096 × 2 int8) must live in SLM, not global memory (2x faster).
  An 18-bit/1 MB table is slower (cache misses).
- XMX: ESIMD `xmx::dpas<8, M, int,int,int,int, s8, s8>(acc, B, A)`; verified layouts:
  B = simd<int,128> as [dword k of 8][lane 16] (lane = weight row, 4 int8 per dword = 4 consecutive columns),
  A = simd<int,8*M> as [m][dword k] (the M input rows, 32 int8 each, uniform), acc/result = simd<int,16*M> as [m][lane].
- ESIMD loops MUST be unrolled at compile time (`unroll<N>` helper with index_sequence + `if constexpr`); with runtime
  loops IGC keeps `w[]` on the stack and it is 3.5x slower.
- `grf_size<256>` property on the ESIMD kernel hung the GPU (engine reset in dmesg). Do not use.
- 16-wide block loads per word were much slower than one 64-dword block load per chunk + `select<16,4>(k)`.
- Results, M=8 rows per decode, exact vs reference: ESIMD 738–808 Gw/s (≈33 ms per pass for GEMM only);
  OpenCL C + `intel_sub_group_i8_i8_matrix_mad_k32` 277–523 Gw/s (≈61 ms); 5060 Ti whole engine ≈35 ms per pass.
- IGC asm dump: `IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=<dir>`.

## Port plan
1. Read the forward pass in `src/engine35.cpp` (pass_body, layer, attn_block, mlp_block, res_norm, gemv) and the kernels
   (`q35_kernels.cu`, `lnord.cu`, `a8_prologue.cuh`, `pm_math.cuh`), write the math down here.
2. SYCL engine as a drop-in `libyskn.so` (same C ABI as `include/yskn_abi.h`) so the Python package (serve/chat/bench/selfcheck) works.
3. Milestone A: 1-row decode, hidden states per layer match the CUDA engine (capture API). B: prefill. C: multi-row verify.
   D: draft model + speculative lookahead (`specx.cpp`, `q35_bdraft.cu`, shead). E: KV5 cache. F: selfcheck PASS + bench.

## Math notes (fill in while reading)

## Status 2026-11-14 (later)
- Specs written by sub-agents (read them before coding): `spec/A_gemv.md` (loader, a8 quantisation, exact int64 GEMM, us/S/r),
  `spec/B_layers.md` (embed, res_norm_xsm, GDN layer, MLP, head/argmax, pm_math), `spec/C_attention.md` (attention + KV5 cache),
  `spec/D_spec_api.md` (ABI, spec_generate loop, drafter, sampler). Default path only; modes listed there.
- `src/gemm.hpp`: engine GEMM (ESIMD, `yx::gemm<BITS, MR>`), exact int64 per-512-block accumulation, validated on real tensors
  (`tests/gemm_ref.py` + `tests/gemm_test`): ~700 Gw/s for MR 1..8, 491 for MR 16, head (8-bit) 620. Layouts: qx `[Kc/32][MR][32]`,
  Aw `[MR][G]` int32, part `[MR][N]` int64, lut uint32[4096], stream = file's warp layout unchanged.
- `ref/core.py` + `ref/run.py`: numpy float32 reference (pm math). Validated vs CUDA oracle `work/oracle/a.npz`
  (`oracle.py`, default config, 12-token prompt): layer 0 has 8 of 12 rows bit-identical, others off by one bf16 ulp in a few
  elements (CUDA rsqrtf/expf last-bit differences) → spec A/B are right. Attention (`Attn` class) is being added by a sub-agent.
- `ref/kv_codec_ref.py`: numpy KV5 codec reference from the spec author.
- The RTX 5060 Ti is in use by ANOTHER session of the user (`..`, bench sweep). Do not run CUDA jobs there
  until it is free (check `nvidia-smi`). An oracle with `ORACLE_ENV=YSKN_Q35_PM=1` (portable math, should be bit-exact vs ref) is
  still to be captured.
- Python strips every `YSKN_*` env var: name port knobs differently (e.g. `YX_*`).
- Selfcheck needs only greedy token ids to match; Python calls open/close/reset/position/last_error/cancel/set_opt/load_bdraft/
  spec_generate/state_hash. Streaming = Python polls `out_ids` (pre-filled with -1).

## Engine design (SYCL)
- One queue, USM device memory, in-order kernel submission; drop-in `libyskn.so` (C ABI of include/yskn_abi.h).
- Non-GEMM kernels: first plain SYCL written for exactness (`-ffp-contract=off`, pm math), unit-tested bit-exactly against
  `ref/core.py` dumps; then fuse/vectorise for speed (kernel-launch overhead matters: ~10+ launches per layer × 64 layers).

## Status 2026-11-14 (engine core)
- `src/kern.hpp` (device helpers, pm math), `src/model.hpp` (loader), `src/engine.hpp` (embed, res_norm, gemm_group, GDN layer,
  state, MLP). Build with `./build.sh tests/<name>`.
- **Layers 0–2 (GDN + MLP), 12-row prefill pass: bit-exact vs `ref/core.py`** (`tests/layers_test 3`, compare `work/xe_h_<l>.bin`
  with `ref_h_<l>.npy`). `tests/stage_test` + `tests/stage_cmp*.py` compare intermediate buffers.
- B60 float divide and sqrt are NOT correctly rounded by default (27% of divisions 1 ulp off). Fix: backend option
  `-Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt"` (in build.sh; user's suggestion; 0 mismatches in 16M, no
  measurable cost). Going through double does not work (compiler folds it back). mul/add/rint/int64→float are exact.
- The SYCL queue must be `in_order`. `to_dev` must be synchronous (callers pass temporaries) — an async memcpy corrupted the first tensor.
- First launch JIT-compiles for ~30 s; set `SYCL_CACHE_PERSISTENT=1` later. 3 layers at M=12 take ~19 ms (non-GEMM kernels are
  naive scalar loops; to be vectorised/fused after correctness).
- Next: attention (reference by sub-agent → SYCL), head + argmax, C ABI + spec_generate (1 row per pass first), then drafter.

## Status 2026-11-14 (API layer, in progress)
- `src/head.hpp` (lm_head GEMM → logits → arg-max with smallest-id tie rule; host-side sampler per spec D 4.2) and
  `src/api.cpp` (C ABI: open/close/reset/position/last_error/cancel/set_opt/load_bdraft(no-op)/prefill/logits/spec_generate;
  one committing row per pass, 16-row prefill chunks, live-prefix reuse mode 1) are WRITTEN BUT NOT YET COMPILED: they need
  `src/attn.hpp` (`yx::Attn(Engine&, layer, max_ctx)`, member `o`, `attn_forward(E, A, M, pos0)`) from the attention sub-agent —
  adapt to whatever interface it actually delivers (see its `spec/C_port_notes.md`).
- Sub-agents running: (1) attention layer in SYCL (`src/attn.hpp`, tests/attn_test), (2) ESIMD vectorisation of the post
  kernels in `src/engine.hpp` (target ≤ 1.0 ms per layer at M=1; was 4.4 ms: GEMMs 0.85 ms + scalar post kernels 3.5 ms).
- Profile before vectorisation (`tests/prof_test 1`): res_norm 0.33–0.5 ms, MLP post ≈ 1.2 ms, GDN posts ≈ 1.2 ms, empty launch 10 µs.
- ESIMD: plain `/` and `sqrt` are inexact, `div_ieee` / `sqrt_ieee` / `rnde` are exact (`tests/ediv_test`).
- Build the library: `icpx ... -shared -fPIC src/api.cpp -o libyskn.so` with the flags of build.sh; run with
  `yasakani selfcheck --model-dir models/speed --dll <lib>` (needs setvars.sh env).
- Then: verify (non-committing M rows) + replay_commit, capture, drafter (spec D 3), speculation loop (spec D 2).

## Goal update (user, 2026-11-14): keep optimising until the B60 is FASTER than the RTX 5060 Ti
Reference: 5060 Ti bench mean 144.9 tok/s (explain 121.7 / code 197.1 / story 71.2 / proof 189.8; 4.2–7.3 tokens per pass,
≈ 35 ms per pass, ctx 8192, greedy, 512 tokens). That needs full look-ahead (drafter) AND a pass of ≲ 35 ms at 8–16 rows.

### GEMM ceiling study (tests/gemm_exp*.cpp, M=8)
- Without the table lookups the kernel reaches 1226–1323 Gw/s = the memory-bandwidth limit; with them 736–808 Gw/s.
  So SLM-gather throughput (≈ 3.5e11 lookups/s) is the bound, not ALU work, thread count or bandwidth.
- No gain from: global-memory (USM) gathers (299 Gw/s), half SLM + half USM (515), 16-bit gathers (same), issuing all 16
  gathers of a chunk before combining and folding the ×4 into the mask (+2%), local size 8..64 (±4%).
- ALL 400 body matrices share ONE value table (identical 4096×2 fp16 table; lm_head has its own). No low-rank structure.
  SLM max is 128 KiB, so a 3-symbol (18-bit, 1 MiB) table giving 4 weights per lookup does not fit in SLM.
- Untested idea: 3D texture 64×64×64 RGBA8 (the 18-bit table) fetched through the sampler (4 weights per fetch, different
  port). Needs image support + XMX in the same kernel (not available in ESIMD; OpenCL C has both).
- Effective code read rate now ≈ 262 GB/s vs ≈ 310 GB/s effective for the CUDA engine on the 5060 Ti (35 ms pass).

## Status 2026-11-14 (ESIMD non-GEMM kernels, `src/fast.hpp`)
- All non-GEMM kernels of `engine.hpp` (embed, res_norm, GDN post/conv/state/out, MLP post) have ESIMD versions in `src/fast.hpp`
  (default). `YX_SCALAR=1` selects the old scalar kernels. Both paths are byte-identical: `tests/fast_cmp <layers> <M1> <M2> <nc2> <out>`
  dumps h, S, xn, us, alpha, qx, ab, z, core, mixed, q/k/v/beta/g, state and conv history over 3 passes; run it with and without
  `YX_SCALAR=1` and `cmp` (done for M = 1, 2, 3, 4, 5, 8, 12, 16, incl. n_commit = 0 and n_commit < M). `tests/fast_test` checks every
  vector helper (rbf, rf16, expf, silu, sigmoid, softplus, logf, rsqrt, int64->float, fwht512, tree128, a8 tail) against the host.
- **ESIMD contracts `a*b+c` into a fused mad by default** (1-ulp errors). Fix: backend option `-ffp-contract=off` next to the divide
  option: `-Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off"` (build.sh). The Engine constructor checks
  it (`fx::esimd_contracts`) and throws if the flag is missing. ESIMD int64->float conversion is exact; division/sqrt only via
  `div_ieee` / `sqrt_ieee`.
- Launches per GDN layer: 19 -> 13 (6 GEMMs + res_norm x2, gdn_post [ab + qkv/z], conv, state [+ history], gdn_out, mlp_post). The four
  "prep" launches are gone: the a8 producers run with one work-group per row (local size G), pass the block scales through SLM, and
  after a barrier thread 0 writes `us[tok]` and `Aw[tok][g]`; `Engine::aw_ready` tells `gemm_group` to skip its prep (it clears the flag).
  Deriving Aw inside every GEMM thread was tried and rejected (exact, but +20 us per GEMM at MR = 8: the GEMM is latency-bound).
- One layer (gdn + res_norm + mlp + res_norm, `tests/prof_test`): M=1 4.4 -> 0.98 ms, M=8 6.1 -> 1.08 ms; GEMMs are now ~0.8 ms of it.

## Status 2026-11-14 (GEMM launch structure)
- Sampler/texture lookups (64³ RGBA8 3D image, 4 weights per fetch; `b60/probe5.py`): exact but 173 Gw/s; the
  fetch alone is that slow (≈ 4.3e10 fetches/s, 8x fewer than SLM gathers). Sampler-less image reads: same. Dead end.
- The GPU runs about **640 ESIMD threads at a time**. A launch with fewer threads leaves it idle (down_proj: 320 threads →
  0.25 ms alone vs 0.116 ms when launches are queued back to back); queued-launch benchmarks therefore overstate the in-engine
  rate (≈ 580 Gw/s in the engine vs 740–800 queued).
- `gemm_multi<BITS, MR>` (gemm.hpp): all matrices of a group in ONE launch, and a lone matrix with < 640 threads is split over
  W = 2 column workers (partials in `E.partw`, summed by a small kernel). `YX_GEMM1=1` = old one-launch-per-matrix path,
  `YX_NOSPLIT=1`, `YX_W=n` for experiments. Bit-exact (layers_test).
- One GDN layer + MLP now: M=1 0.80 ms, M=8 0.89 ms, M=16 1.04 ms (was 4.4 / 6.1 ms). ×64 ≈ 51 / 57 / 67 ms per pass before
  attention, head and drafter. 5060 Ti: ≈ 35 ms per pass. Still ~1.6x behind per pass.

## MILESTONE 2026-11-14: selfcheck PASS on the B60
- `src/attn.hpp` (attention + KV5 cache, ESIMD, by sub-agent; notes in `spec/C_port_notes.md`; bit-exact vs ref incl. split passes,
  INT8-V layers, context crossing 4096; tree passes not implemented, M ≤ 16).
- Library: `lib/libyskn.so` built from `src/api.cpp`:
  `icpx -fsycl -O2 -std=c++20 -ffp-contract=off -fno-fast-math -Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off" -I ../third_party/nlohmann -shared -fPIC -o lib/libyskn.so src/api.cpp`
- `source /opt/intel/oneapi/setvars.sh; SYCL_CACHE_PERSISTENT=1 yasakani selfcheck --model-dir models/speed --dll lib/libyskn.so --max-ctx 8192`
  → short-answer / thinking (160 tokens) / plain-text all PASS = same greedy ids as the CUDA engine.
  (The CLI's "GPU memory almost full" warning and "GPU:" line come from nvidia-smi = the 5060 Ti, not the B60. Ignore.)
- `yasakani bench --n 128`: 18.2 tok/s on all four tasks, 1.01 tokens/pass (no look-ahead yet) → ≈ 55 ms per 1-row pass.
- Next for speed: look-ahead (verify M rows without commit + replay_commit, capture, drafter, loop of spec D 2/3), then profile.

## Status 2026-11-14 (look-ahead base + pass profile + more lookup experiments)
- api.cpp: `verify` (M ≤ 16 rows, non-committing), `commit` (state replay), host drafts D0/D1, non-drafter loop.
  selfcheck PASS with look-ahead on; bench --n 256: 20.6 tok/s mean, 1.09–1.35 tokens/pass. `YX_NO_LOOKAHEAD=1` disables it.
- Whole pass (`tests/pass_prof 256`, context 256): M=1 53.4 ms, M=8 61.2 ms, M=16 73.1 ms (+ head 2.2 ms for all rows).
  Breakdown M=1: MLP 30.5 ms, GDN 13.9, attention 6.6, res_norm 4.5 (128 launches). In-engine GEMM rate ≈ 570–625 Gw/s.
- More workers for grouped matrices (YX_WG=2/5/10): slower. Keep W=1 for groups, W=2 for lone low-row matrices.
- Lookup alternatives, all worse than SLM gathers (2.4–3.5e11 lookups/s):
  USM gather with cache hints L1/L2 cached: 299 Gw/s (same as without); 18-bit 1 MiB table in USM, 4 weights per gather: 196 Gw/s;
  table held in registers + `simd::iselect` (register-indirect, no memory): 1.1–1.3e11 lookups/s (tests/isel_test.cpp), i.e. 2x
  slower than SLM gathers, so the large-GRF (256 registers) idea is not worth the risk; sampler/texture: 4.3e10 fetches/s.
  Conclusion: ≈ 600–750 Gw/s is the practical GEMM ceiling on this GPU for this weight format (bandwidth alone would allow 1200+).
- Consequence: a main-model pass on the B60 is ≈ 50–75 ms vs ≈ 35 ms on the 5060 Ti. With equal tokens/pass the B60 lands at
  roughly 0.5–0.65x of the 5060 Ti. A sub-agent is porting the drafter (src/draft.hpp + loop in api.cpp).

## Drafter "bdraft2" + drafter branch of the generation loop (2026-11-14, `src/draft.hpp`, `src/api.cpp`)
Implemented: capture of `E.h` after layers 5/19/33/47/61 for every row of every pass (`yskn_engine::pass`, one device memcpy per
capture layer, only when the drafter is loaded), `yskn_load_bdraft` (bdraft2_k8.yskn + SHEAD head from `$YSKN_BDR_SHEAD`, else
`<draft>.shead`; `$YSKN_BDR_SHEAD_M` = 512, max 1024), context path + rings, one-shot draft block (8 or 16 rows), SHEAD head,
candidate selection (greedy and the Gumbel branch, cpl = 850), and the loop of spec D 2.4 (drafter vs D0 running averages `dema`,
bdr_adapt / bdr_probe, pending comparisons, 8/16-row controller b16_*, catch-up after every commit, flush at the end, stats
23/24/25/26/28/37/38/39/40). No-drafter paths are unchanged (cfg bdraft = 0, `YX_NO_LOOKAHEAD=1`, or load_bdraft never called).
NOT ported: HCAP side candidates / row tables / leaf promotion (spec D 3.5) → runs with hb = false. In CUDA the HCAP search only
runs when sampling (cpl > 0), so greedy (bench, selfcheck) loses nothing; sampling loses the leaf-hit bonus token. Snapshots
(mode 2) are not in the port, so the rings are not snapshotted either.

### Layouts (all in `yx::Draft`)
- `cap` f32 `[5 captures][16 rows][5120]`; rows of the last pass stay valid until the next drafter step (catch-up uses rows 0..a).
- GEMM: the port's `gemm_multi<8, MR>` with MR = 8 (≤ 8 rows) or 16, own scratch (`qx` 16×25600, `alpha`/`Aw` 16×50, two `us`
  buffers used alternately because a producer of row m also reads `us[m-1]` of the previous product). All 46 matrices share ONE
  value table and the signs of `bdraft2.fc` (seed 7, offset 0 = the body signs). A launch needs `threads % 32 == 0`
  (akp/mkp have 80 threads → W must be even); W column workers are chosen to reach ≈ 640 threads, partials summed by a small kernel.
- Rings `rk`, `rv`: bf16 `[5 layers][8 KV heads][128 key groups][2048]` in XMX layouts — position t → group `(t & 2047) >> 4`,
  lane `t & 15`; keys `[dim chunk 8][dim pair 8][key lane 16][2]`, values `[dim chunk 8][key pair 8][dim lane 16][2]` (`dx::kix/vix`).
  `ring_lo/ring_hi` = range of positions that are valid in the rings (reset → empty; a pass committed without drafter
  context leaves a hole and the next block simply attends less context), so cfg bdraft = 0 passes cost nothing.
- Attention: bf16 × bf16 → f32 `xmx::dpas` (verified: `tests/dpas_bf_test`): per 16 keys, 8 products give the scores of 2 query
  heads, 8 more the weighted values (queries and softmax weights rounded to bf16; block rows' own k/v also as bf16).
  Threads = (part, row, head pair); parts merged in `k_a8att`. Full 2048 window: 0.46 ms per block (was 2.8 ms with float code).
- Rope: host table `[max_ctx + 16][cos 64 | sin 64]` (134 MB at max_ctx 262144). Frontier / mask embedding: `fx::k_embed` row.
- SHEAD: `a` (V×512 int8) re-laid out at load for XMX (`[16 tokens][32-column chunk][dword 8][token lane 16]`); the 512
  projections of a row are quantised to 16 bit (signed high byte + unsigned low byte → two s8×s8 / s8×u8 dpas), so the low-rank
  score is exact to 3e-5. Top-512: 2-level histogram of the monotone float key (10 + 11 bits, SLM atomics per work-group, verified by
  `tests/atom_test`), all keys above the 21-bit threshold are taken plus all ties if they fit in 1024 (deterministic SET; the slot
  order is racy but the top-16 by (value desc, id asc) does not depend on it). Re-scoring with the int8 lm_head copy on the device;
  top-16 + rank-256 `sel` correction + greedy / Gumbel choice on the HOST (`pred`/`succ` stay in the mapped file; one read-back of
  ≈ 130 KB per draft).
- Sampling: same `gumbel(seed, pos, token)` as `Head::sample`; ids with sampling are identical with and without look-ahead.

### Test commands
    ./mklib.sh lib/libyskn_d.so          # private copy (mklib.sh now takes an optional output name); a build takes 3 min
    yasakani selfcheck ... --dll lib/libyskn_d.so                                  # PASS with the drafter active
    python tests/la_check.py --dll L --tokens 384 [--sample] [--reuse]             # off / drafter / D0-D1-only: identical ids
    python tests/la_check.py --dll L --tokens 384 --dump A; YX_NO_LOOKAHEAD=1 ... --dump B; cmp A B     # env switch variant
    YX_DSTAT=1 [YX_DPROF=1|2] yasakani bench ...      # per-call stats on stderr; DPROF=1 waits per stage, 2 = per kernel kind
    knobs: YX_BLK16=0|1|2 (8-row only / always 16 / adaptive), YX_DW=n (max GEMM workers), YX_DNP=n (attention parts)

### Results (B60, ctx 8192, greedy, `bench --n 512`; two runs identical: drafts are deterministic)
| task | B60 tok/pass | B60 tok/s | CUDA 5060 Ti tok/pass | CUDA tok/s |
|---|---|---|---|---|
| explain | 5.28 | 72.1 | 4.2 | 121.7 |
| code | 6.56 | 86.3 | 7.3 | 197.1 |
| story | 2.65 | 37.1 | 2.4 | 71.2 |
| proof | 6.56 | 86.4 | 7.1 | 189.8 |
| mean | 5.26 | 70.5 (was 20.6) | 5.25 | 144.9 |
- Gates: selfcheck PASS; 4 tasks × 384 tokens identical ids for look-ahead on / off (cfg and env variant), also with sampling
  (T 0.7, seed 1234) and for the live-prefix continuation (mode 1). Sampling: 8.1 tok/pass on `code` (Gumbel branch works).
- Long prompt (3134 tokens, ring wrapped): summary 3.84 tok/pass; verbatim-quote task 14.2 tok/pass (D0 replaces the drafter in 23
  of 27 passes, 5 probes) → dema / probe / 16-row logic is alive.
- One pass (explain, 97 passes, `YX_DPROF=1`): verify (main model 8–16 rows + head + read-back) 64.3 ms, state replay of the commit
  3.0 ms, drafter 5.9 ms = context catch-up 0.50 + block 4.4 + head 0.79 + host select 0.22. Without profiling waits
  drafter + replay = 8.6 ms per pass. Per kernel kind (ms per step, waits included): GEMM 4.1 (1.84 G weights, i.e. the 8-bit GEMM
  rate ≈ the 7 % of the main model's weights), norm 0.37, qkv 0.31, score 0.33, dyn 0.22, glu 0.18, hist 0.17, a8att 0.17, rescore 0.13,
  attn 0.10 (0.46 at the full window).

### Open issues / ideas
- The main-model verify pass (64 ms at 8 rows) is 88 % of a pass; the drafter is 8 %, the replay 4 %. tok/pass equals CUDA's on
  average (per task ±10–25 %, no CUDA oracle was available to compare drafts).
- State replay = 48 `k_state` launches, 1.8 ms (5 rows) … 7 ms (16 rows) per pass: a single launch over all 48 linear layers
  (or keeping the per-row states of the verify pass and copying row a) would remove most of it. That kernel lives in fast.hpp.
- HCAP + row tables + leaf promotion (needs tree rows in attn.hpp), snapshots (mode 2) incl. the 10 rings.
- Drafter GEMM launches with few threads (akp/mkp 80, k/v 64 per matrix) are latency-bound; fusing akp+mkp is not possible (different
  inputs). `k_qkv`/`k_ring` are plain SYCL (0.06 ms per launch); an ESIMD version would save ≈ 0.2 ms per step.
- `picks()` with sampling reads all logits of M rows to the host (1 MB per row): slow for sampling; a device top-20 would fix it.

## Status 2026-11-14 (drafter done; quality model)
- Drafter + drafter loop ported by sub-agent (`src/draft.hpp`, api.cpp; section above). Speed model bench --n 512:
  explain 72.1 / code 86.3 / story 37.1 / proof 86.4, mean **70.5 tok/s**, 5.26 tokens/pass (CUDA 5060 Ti: 144.9 tok/s, 5.25 tok/pass).
  selfcheck PASS with drafter; ids identical with look-ahead on/off. HCAP and snapshots (mode 2) not implemented.
- Quality model (`models/quality`, 288 4-bit + 112 3-bit body matrices): `Tensor::lut_id` + `gemm_group` now split a
  group by (bits, value table). `attn_default_profile` falls back to `$YSKN_TB_PROFILE`. Reference runs it with `YX_REF_QUALITY=1`
  (w8 cache in work/w8q); `YX_MODEL=<file> tests/layers_test 3` is BIT-EXACT vs the reference for layers 0–2.
  Whole pass: 55.8 / 62.5 / 73.6 ms at 1 / 8 / 16 rows (ctx 64). bench --n 512: explain 54.0 / code 91.1 / story 34.1 / proof 77.9,
  mean **64.3 tok/s**. It does not fit the 16 GB 5060 Ti with the drafter.
- OPEN: quality selfcheck: short-answer PASS, plain-text PASS, **thinking (160 tokens) FAIL** (same with YX_NO_LOOKAHEAD=1).
  Cause not determined: near-tie flip from last-bit math differences vs CUDA (rsqrtf/expf) or a bug in a path only this model
  uses beyond layer 2 (e.g. attention q/k/v with mixed widths). Needs a CUDA run of the quality model (5090, or the 5060 Ti
  without drafter at small ctx when it is free) to find the first differing token.
- OPEN: one GPU page fault ("Fault response: Unsuccessful" in dmesg) happened during a quality selfcheck WITH look-ahead
  (an earlier build hung in a memcpy at the same stage); not reproduced in the following bench run. Suspect the drafter path.

## Investigation 2026-11-14 (quality selfcheck mismatch, GPU fault) — both explained
- Quality "thinking" mismatch = ONE near-tie. `tests/flip_search.py <model dir> <lib> thinking` (uses `YX_MARGIN_LOG`, a debug log
  of top-2 logits per position in api.cpp `picks`): our 160 ids have the reference's first 12; the smallest top-2 margin is
  0.0146 at index 28 (id 16 vs 220); replacing that one token by the runner-up and continuing greedily gives exactly the
  reference sha256. Next smallest margin is 0.247. So it is last-bit math drift vs CUDA (rsqrtf/expf), not a port bug.
- GPU faults in dmesg: (a) 09:48:57 compute-engine (ccs) page faults = start of the run with a library built from the drafter
  sub-agent's UNFINISHED sources (that run then hung in a USM memcpy); (b) 10:25:23 copy-engine (bcs) fault + reset = the moment
  the hung process was killed. With the finished build: 5 quality selfcheck runs (with and without look-ahead) + bench, 0 faults.

## Attention: decoded mirrors + XMX products (mirror path)

- Problem: `attend` decoded every key from its 5-bit code once per (row, head group): 32 times per pass with 16 rows.
  One pass grew by 5.8 ms per 1,000 tokens of context (60,000 tokens: 414 ms per pass, prompt read 45 tokens/s).
- Now (`AttnShared::mir`): `select` also writes each new key and value decoded to bf16 into mirrors in XMX layouts
  (`Attn::mk`: `[4][G][16 dim chunks][8 dim pairs][16 keys][2]`, `Attn::mv`: `[4][G][16 dim chunks][8 key pairs][16 dims][2]`,
  4,096 bytes per token and layer for both, 4.3 GB at 65,536 tokens). `post` writes the queries as bf16 (`qa`).
  `score<QS>`: thread = (KV head, QS groups of 8 queries, part of `A_PS` = 512 keys): bf16 `dpas` scores, maximum of the part,
  p = exp(score - max), sum, weights `rbf(p * vs)` as bf16 (`pa`). `vacc<VQ, VC>`: weighted values by `dpas`, + mean * sum(p).
  `combine` merges the parts (same code as for the 42 splits). Parts depend on the absolute position only, so a row's result
  does not depend on the pass it is computed in (look-ahead on/off stays identical).
- Not bit-identical to the old path (order of the sums). `YX_ATTN_OLD=1` selects the old path (the bit-exact tests against
  `ref/` need it); it is also used when the mirrors would need more than `YX_MIRROR_GB` (default 10) GB.
  Knobs: `YX_PSH` (log2 of the part size, 9), `YX_SV` (QS, VQ, VC digits, 224).
- `tests/pass_prof <context> <max_ctx>`, speed model, 16 rows, ms per pass: 256: 73; 4,096: 77; 20,000: 84; 60,000: 126
  (attention 13 / 17 / 24 / 66). 1 row: 56 / 58 / 60 / 72. Self-check: speed model 3 of 3, quality model as before.

## Prefill batches, state kernel, decode-once GEMM, work-group attention (November 15)

Baseline before this work (speed model, context 65,536): prompt read 201 / 179 / 161 tokens/s at 8,000 / 30,000 / 60,000 tokens,
bench mean 65.2 tokens/s, 256-row pass not possible (16 rows per pass, 4.6 ms per token).

- **Prefill batch** (`pf_batch()`, `YX_PF_BATCH` = 16 / 64 / 128 / 256, default 256; `Engine::cap` sizes all scratch buffers; `MRMAX` = 16
  stays the decode / verify limit). `feed()` commits `cap` rows per pass. Look-ahead needs the model's choice after every prompt row,
  so the head also runs on the whole batch (`Head` buffers are sized by `cap`); without look-ahead only the last row goes through the
  head (`row_to_front`). Drafter context: captured rows go to `pfcap` and are fed to the drafter in its own 16-row chunks.
  512 rows do not fit: with mirrors and batch buffers the card runs out of memory (prompt read fell to 48 tokens/s).
- **GEMM for batches** (`gemm.hpp`): rows are free only up to 16 per decode (the products hide behind the gather latency).
  `gemm_big<BITS, TR>` (row-set threads, input rows in SLM, sums in `part`): 64 rows of gate+up 0.82 ms with 32 rows per thread
  (16 per thread 1.22, 64 per thread 1.16 = register spills). Used for 64 rows.
  `gemm_once<BITS, D, ND>` (from 128 rows): a work-group of 16 threads owns 16 output rows, every thread decodes ONE of the 16
  column chunks of a 512-column group into SLM, then multiplies all 16 with its own 4 / 8 / 16 input rows, int64 sums in registers.
  One decode per launch instead of one per 32 rows: 256 rows gate+up 2.72 -> 1.39 ms, qkv+z 1.32 -> 0.66, down 1.31 -> 0.71.
  Dead ends: sums in memory / SLM per row set with a private stash of the decoded weights (3.7 x slower), 64 sums in registers per thread.
- **DeltaNet state** (`fx::k_state`): the recurrence is serial in the rows. A (head, 16 columns) block is shared by 2 threads that
  keep 64 state rows each in registers and exchange the 4 partial sums through SLM (2 barriers per row); same operations and order.
  0.50 -> 0.10 ms per token; the 16-row decode pass went from 75.9 to 70.0 ms. (State in SLM, one thread: no gain.)
- **Attention** (`score16`, `vacc16`, default `YX_SV=550`): a work-group shares the 16-key tile of K (V) through SLM, each thread owns
  8 queries (queries in registers) resp. 8 queries x 8 dim chunks of sums. Same values as `score<QS>` / `vacc<VQ, VC>`.
  60,000 tokens, 256 rows: scores + values 997 -> 425 ms; 16 rows: 54 -> 32 ms. Work-groups that use the same (KV head, part) are
  neighbours in the dispatch order.
- **Checks**: `yskn_state_hash` (DeltaNet states, conv histories, KV codes, mirrors) + `tests/pf_check.py` (prompt lengths 16 ... 3000
  around the batch boundaries, several chunks in a row): batches 16 / 64 / 128 / 256 give the hashes and first tokens of the original
  16-row kernels. `tests/gemm_big_test` (GEMM timing + checksum), `tests/pass_prof` with `YX_FINE=1` (per-stage times, `Engine::mark`).
- 256-row pass at context 512: 656 -> 380 ms (GEMM 1.48 -> 0.80 ms per token). Per token at 256 rows: GEMM 0.80, attention encode 0.18,
  MLP post 0.15, GDN post 0.12, state 0.10 ms.

### Results and dead ends of November 15 (speed model, context 65,536, batch 256, `lib` built from this tree)

Conditions: Arc Pro B60, oneAPI DPC++ 2026.2, Level Zero, greedy, look-ahead on, one job on the GPU at a time (`flock work/b60.lock`;
two engine instances do not fit in 24 GB and thrash). Prompt read / generation from `bench/longctx_b60.py` (256 generated tokens),
decode from `yasakani bench` (512 tokens, 1 run). The RTX 5060 Ti figures are the other card's saved results.

| | before | now | RTX 5060 Ti |
|---|---|---|---|
| prompt read, 8,000 tokens | 201 tok/s | 550 tok/s | 792 |
| prompt read, 30,000 tokens | 179 tok/s | 486 tok/s | 720 |
| prompt read, 60,000 tokens | 161 tok/s | 419 tok/s | 643 |
| generation after 8,000 / 30,000 / 60,000 | 44.1 / 36.2 / 30.3 | 49.1 / 38.9 / 35.0 tok/s | 99.1 / 96.1 / 76.5 |
| decode, mean of the 4 bench prompts | 65.2 tok/s | 71.7 tok/s | 149.1 |

Checks passed with this build: state hashes and first tokens of batches 16 and 256 equal to the original 16-row kernels;
self-check speed model 3 of 3, quality model as before (the one near-tie in "thinking"); look-ahead on / off identical ids on 4 prompts.

256-row pass, ms per token by context (`tests/pass_prof <context> 65536`, `YX_FINE=1`):

| stage | 8K | 30K | 60K |
|---|---|---|---|
| GEMM (all) | 0.80 | 0.80 | 0.80 |
| attention scores + values, per-thread K/V loads (`YX_SV=224`) | 0.27 | 1.09 | 3.90 |
| ... work-group tiles in SLM (`score16` / `vacc16`), groups of a part far apart | | 0.78 | 1.66 |
| ... the same, groups of a part adjacent in the dispatch order (default) | | 0.62 | 1.22 |
| ... fused online softmax (`YX_ATTN_FA=1`) | | 1.36 (2.26 before the register fix) | 4.48 (before the fix) |
| attention other / GDN without GEMM / MLP post + res_norm | 0.22 / 0.28 / 0.19 | 0.26 / 0.28 / 0.19 | 0.32 / 0.28 / 0.19 |

Why the order of the work-groups matters: at 60,000 tokens the K mirror of a layer is 123 MB and a part (512 keys of one KV head)
is 256 KB. The 12 work-groups that use the same part were 118 parts (about 30 MB) apart in the dispatch order, more than the
second cache level holds (16-32 MiB measured in bench/architecture), so every logical read of a tile was a DRAM read. With the
groups adjacent the tile is read from memory once. (Inferred from the timings; DRAM bytes and cache hit rates were not measured:
no VTune / GPU counters were used.)

Dead ends (kept in the source only where noted):
- `gemm_big` with 64 rows per thread (64 int32 sums in registers): 1.16 ms against 0.82 ms with 32 rows per thread: spills.
- Decode once with a per-thread stash of the decoded weights in SLM and the int64 sums in `part` (removed): 3.7 x slower than
  `gemm_big`; sums in memory or SLM are slow, they have to stay in registers (`gemm_once`).
- USM loads of the input rows in `gemm_big`: about as expensive as the products; the rows go to SLM.
- DeltaNet state in SLM with one thread per block: no gain; registers + 2 threads: 4.7 x.
- `score1` (`YX_SV=3xx`, queries in registers, per-thread K loads): 5 % at most; superseded by `score16`.
- Batch 512: out of memory with the mirrors (prompt read 48 tokens/s). `pf_batch()` caps at 256.
- Fused attention (`ak::fa48`, `YX_ATTN_FA=1`, experiment): scores, online softmax and weighted values in one kernel, 48 threads
  per work-group (16 score threads + 32 value threads), no score / weight buffers in global memory. 3.6 x slower than the default
  at 512 / 30,000 / 60,000 tokens in the first version: both roles' registers (64 + 64) were allocated in every thread. With one
  shared block of 64 registers it is still 2.2 x slower at 30,000 tokens (348 against 159 ms per 256-row pass): three barriers per
  round of 64 keys with two thirds of the threads idle in each phase, and scalar code per query for the running maximum.
  Not bit-identical to the default path (maximum per group of 16 keys instead of per part); its numerical error was not evaluated
  because it is not a candidate. The default stays `score16` / `vacc16`.

### Device memory, automatic prefill batch, generated code (November 15)

**Allocations by purpose** (`YX_MEM=1` prints them at open and after the draft model is loaded; context 65,536; GB):

| model, batch | weights + engine scratch | head | attention (KV cache 1.4, mirrors 4.3, batch buffers) | draft model | allocated | free (driver) |
|---|---|---|---|---|---|---|
| speed, 128 | 9.08 | 0.95 | 7.65 | 2.29 | 19.97 | 2.34 |
| quality, 64 | 10.63 | 0.77 | 6.97 | 2.29 | 20.66 | 1.65 |
| quality, 128 | 10.83 | 0.95 | 7.81 | 2.29 | 21.88 | 0.41 |
| quality, 256 | 11.23 | 1.30 | 9.50 | 2.29 | 24.32 | 0.00 |

- The card reports 23.91 GB; allocated + free is 22.3 GB in every row, i.e. that is what an engine can use.
- **Quality model with batch 256: 24.32 GB allocated, 0.00 GB free: over-committed.** The driver keeps running and moves buffers
  to system memory; generation falls to 0.3-0.4 tokens/s (9 s per pass), prompt reading is still 461 / 386 / 372 tokens/s.
  Batch 128: 482 / 378 tokens/s at 8,000 / 60,000 tokens and 49.3 / 29.9 tokens/s after them; batch 64: 335 / 280 and the same
  generation. So 128 is the right batch for the quality model, and a larger batch is not faster once memory is tight.
- A row of batch costs 19 MB at this context (64 -> 128: +1.22 GB, 128 -> 256: +2.44 GB).
- **Automatic batch** (`plan_batch` in `api.cpp`; `YX_PF_BATCH` overrides it): engine + head + attention are constructed twice in a
  counting mode (`yx::g_dry`: `to_dev` / `Engine::dev` only add up bytes) for batches 16 and 256; the need is linear in the batch.
  need(batch) = counted bytes + 0.2 GB (the count comes out 0.13 GB low in all 9 measured configurations) + 0.9 x the size of the
  draft-model files next to the model (2.29 GB allocated for 2.57 GB of files). The largest of 256 / 128 / 64 / 16 with
  need + 0.15 GB (`YX_MEM_MARGIN_GB`) <= the device memory that is FREE at start is taken (`ZES_ENABLE_SYSMAN=1` is set by the
  library so that the driver reports it; other processes' memory is already taken off; fallback 93 % of the total).
  Estimate / allocated (GB): speed 16: 17.89 / 17.81, 64: 18.75 / 18.68, 128: 19.91 / 19.84, 256: 22.23 / 22.16; quality 16: 19.80 / 19.73,
  64: 20.67 / 20.60, 128: 21.83 / 21.76; quality 256: 24.27 (does not fit 22.43 free). Chosen: speed 256, quality 128.
  The speed model with 256 leaves 0.17 GB free: a little less free memory at start and it gets 128 (491 / 385 instead of
  551 / 423 tokens/s at 8,000 / 60,000 tokens).
- **Allocation failure**: `yskn_open` frees what the failed attempt allocated (`yx::g_dev_ptrs`, `dev_release`) and retries with the
  next smaller batch. Test: `YX_FAIL_ALLOC_GB=19.5` -> "batch 256: out of device memory", then batch 128 with 17.55 GB allocated
  (exactly the engine at 128: nothing left from the first attempt) and the reference state hashes. A failure while the draft model
  is loaded is reported as an error (no retry there). `yskn_close` now frees the engine's device memory.
- The partial-sum buffers of the old attention path (0.27 GB at batch 256) are no longer allocated with mirrors.
- Not done: freeing the prefill buffers after a prompt, or separate buffer sets for prefill and decode. It would not make anything
  faster (with batch 256 the quality model is already short of memory while reading), and the attention buffers are laid out by the
  batch capacity, so decode would need its own set and every attention / head kernel a switch. Worth doing only to share the card.

**Generated code** (IGC assembly, `NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=<dir>/`,
counted with bench/architecture `scripts/igc_asm_count.py`; all kernels numGRF = 128). Measured from the assembly:
- `score16`, `vacc16`, `gemm_once`, `gemm_big` with 32 rows per thread: no register spills. XMX is used: 16 bf16 `dpas` per group of
  16 keys in `score16`, 8 in `vacc16`, 32 int8 `dpas` per column group in `gemm_once<6, 8, 2>`.
- `gemm_big` with 64 rows per thread: 3,456 bytes of spill, 208 spill stores and 181 fills in the loop body (512 `dpas`): the reason
  why it is slower than 32 rows per thread.
- `fa48` (fused attention experiment): after the shared register block only flag spills are left (12 stores, 25 loads); it is still
  2.2 x slower than the default, so registers were one cause but not the only one.
Not measured directly: occupancy, cache hit rates, DRAM bytes (inferred from timings only).

**Barriers in `score16` / `vacc16`**: `A_RG` = 4 groups of 16 keys are staged per round (2 barriers per round instead of per group).
60,000 tokens, 256 rows: values 136 -> 123 ms, scores 180 -> 178 ms. All SLM writes of a round happen before its first barrier and
all reads between the two barriers; the state hashes are identical to the original kernels and between repeated runs.

**Final figures of November 15** (context 65,536, automatic batch; `lib` of this tree; one job on the GPU):

| | speed model (batch 256) | quality model (batch 128) | RTX 5060 Ti, speed model |
|---|---|---|---|
| prompt read 8,000 / 30,000 / 60,000 tokens | 551 / 489 / 423 tok/s | 482 / 434 / 381 tok/s | 792 / 720 / 643 |
| generation after them | 48.8 / 38.5 / 34.6 tok/s | 49.0 / 38.1 / 29.5 tok/s | 99.1 / 96.1 / 76.5 |
| bench mean, thinking off / on (512 tokens, 2 runs) | 71.8 / 63.8 tok/s | 69.2 / 62.4 tok/s | 149.1 / 123.6 |
| self-check | 3 of 3 | 2 of 3 (the known near-tie) | 3 of 3 |

State hashes: batch 16 = automatic batch for both models; speed model = the original 16-row kernels; repeated runs identical.
Look-ahead on / off: identical ids on the 4 prompts, both models.

- KV profile default (found when the repository build was checked without the Python package): it was a path relative to the
  working directory (and the speed model's file); now `kvq_u5.bin` next to the model file, after `$YX_KV_PROFILE` and
  `$YSKN_TB_PROFILE`. The profile files of the two models are byte-identical, so the earlier `pf_check` runs of the quality model
  (which do not go through the package and used the speed model's file) remain valid.

## Decode speed (November 15, second round): profile, limits, what changed

Speed model, context 65,536, greedy, `bench --n 512`. Target was 90 tok/s; reached 83.5 (from 71.8).

**Profile of one pass before the changes** (8 verify rows, short context, `explain`, 4.16 tokens per pass):
verify pass of the main model 59.8 ms (88 %) = GEMM 43, GDN non-GEMM 5.8, attention non-GEMM 6.7, res_norm 4.6, MLP post 3.4, head 2.2
(stage values include profiling waits); draft model 6.5 ms per block of 8 or 16 tokens (GEMM 4.1); state replay 1.9 ms (48 launches).
About 870 launches per pass x 3 us = 2.6 ms. Host waits: 3 to 4 per pass (ids, head read-back, drafter read-back), nothing per kernel.
Rows: 1 row 54.4 ms, 8 rows 58.1, 16 rows 65.4 (+ head 2.2). Look-ahead off: 55.8 ms per token = 18.1 tok/s.

**The decode GEMM is bound by table lookups, not by bus, XMX or registers** (`bench/architecture/b60/gemm_decode.cpp`, exact checks):
- rate in the engine 530 to 630 G weights/s = 2.7 to 3.1 x 10^11 lookups/s at every row count 1..16; bus use 230 of 435 GB/s, XMX 1 to 17
  of 196 TOPS, ALU 40 %, no spills, launch 3 us of 150 to 300.
- code blocks requested one trip ahead: +-3 %; the 16 lookups of a chunk requested before use: +-3 % (the gains seen at 16 rows in
  the micro-benchmark, 4 to 6 %, did not appear in the engine: `YX_G16=20|2`, kept as a test switch); half or a quarter of the lookups
  from a copy of the table in global memory: 58 % / 33 % slower; one 32-lane message instead of two 16-lane ones: no change (the
  assembly shows a real 32-lane `load.slm`), so the cost is per address.
- the value table (one for all matrices of the speed model) has no structure: 3708 distinct pairs in 4096 entries, full rank.
- 12.8 G lookups per pass at 3.0 to 3.2 x 10^11/s = 40 to 43 ms: the GEMM of a pass cannot go below about 40 ms with this weight format.

**Changes** (all default; outputs unchanged):
1. Rows per pass: the reference controller (thresholds of the CUDA engine) used 16 rows too rarely. New cost-based rule (`YX_BLK16=3`,
   default; 2 = old): 16 rows when the tokens they add per pass exceed (c16 - 1) x the tokens of an 8-row pass, c16 = 1.12 measured
   (`YX_B16_COST`); the gain is observed in 16-row passes and estimated in 8-row passes from fully accepted blocks. 8 rows only 70.4,
   old controller 71.8, always 16: 79.3, new: 82.7 tok/s. (c16 = 1.06: 82.4, 1.2: 80.6.)
2. Memory access of the non-GEMM kernels in 256-byte pieces instead of 64 (a load costs a thread about 50 ns whatever its size):
   res_norm 4.57 -> 3.64 ms, MLP post 3.35 -> 2.87, GDN out 1.52 -> 1.32; 8-row pass 58.1 -> 56.5 ms.
3. Deferred state replay: the accepted rows of a look-ahead pass are run at the start of the next pass's state kernel (second
   16-row buffer set for q, k, v, beta, g per layer, +32 MB); 48 launches and 48 reloads of the states less. `YX_NO_DEFER=1` = old.
   Flushed before prompt reading, state hashes and reset. About 1 % of a pass.
4. `down_proj` with 4 column workers at 16 rows (`YX_W16`): 11.4 -> 10.2 ms; 16-row pass 65.4 -> 62.9 ms. (Slower at 8 rows: not used there.)

**Checks** (`tests/dec_check.py`: ids, positions and the four state hashes after generation and after a prefix-reuse continuation,
greedy and sampling): new build = pushed build = deferral off = always 16 rows = look-ahead off, both models. `pf_check`: batch 16 =
automatic batch = pushed build (both models) = original 16-row kernels (speed). `la_check --sample --reuse` PASS both models. Self-check
speed 3/3, quality 2/3 as before. Automatic batch unchanged: speed 256 (22.19 GB allocated, estimate 22.26), quality 128 (21.79 / 21.86).

**Results** (tok/s; before -> after)
| | speed | quality |
|---|---|---|
| bench, thinking off (mean of 4 prompts, 2 runs) | 71.8 -> 83.5 | 69.3 -> 77.5 |
| per prompt explain / code / story / proof | 64.4 / 114.3 / 40.5 / 114.9 | 57.1 / 115.6 / 37.6 / 99.7 |
| bench, thinking on | 63.8 -> 66.5 | 62.4 -> 65.6 |
| look-ahead off | 18.1 -> 18.6 | 17.7 |
| after an 8K / 30K / 60K prompt | 48.8 / 38.5 / 34.6 -> 49.3 / 39.3 / 35.2 | 49.0 / 38.1 / 29.5 -> 50.5 / 38.6 / 29.7 |
| the same, look-ahead off | 17.5 / 15.1 / 12.9 | 16.7 / 14.5 / 12.5 |
| prompt reading 8K / 30K / 60K | 551 / 489 / 423 -> 553 / 490 / 424 | 482 / 434 / 381 -> 487 / 437 / 384 |

**Why not 90.** A pass is now about 65 ms (8 rows) to 72 ms (16 rows): GEMM 43 (floor 40), draft model 6.9, the rest 15 to 22. 90 tok/s
at the present tokens per pass needs 60 / 67 ms... i.e. about 8 % less, all of it from the non-GEMM 22 ms. Left to do there: the cache
write kernel `select` is scalar SYCL (1.6 ms per pass), trellis encode 2.1 to 3.6 ms, GDN post at 16 rows 3.2 ms (2048 threads),
drafter GEMM at 449 instead of about 600 G weights/s (1 ms), 48 history launches at commit (0.15 ms). After long prompts the text of
the test is accepted at 3.1 to 3.5 tokens per pass and attention adds 14 to 24 ms per pass, so those figures barely moved.

## ISA-guided round (November 16): non-GEMM kernels, division modes, fused attention experiments

Speed model unless noted, context 65,536, one job on the B60 at a time (a lock file). Baseline re-measured before any change: bench 83.5 / 83.5
tok/s (two runs).

### Tools
- `tools/isa_report.py <dump dir> [--diff <dir2>] [--match ..] [--csv ..] [--md ..]`: per-kernel table from an IGC dump (GRF, static
  instructions, GRF spills / fills = instructions the compiler annotated as spill / fill on the scratch surface, flag spills from the
  header, dpas, block / gather loads and stores to global memory and to SLM, barriers, gateway sends, branches, SLM bytes from zeinfo).
  Counts are static. Ordinary `load.ugm` is never counted as a spill.
- `tests/np_prof.cpp`: per-launch time of the non-GEMM kernels back to back (one wait per 400 launches), empty launches of the same
  shape, the MLP kernel cut after each stage, instruction-chain experiments. CAUTION: its inputs are whatever the last GEMM left in
  the shared scratch (`part`), not the inputs of that kernel, so data-dependent costs (div_ieee early-outs, fallback rates) are not
  those of a real pass; use `tests/pass_prof` (`YX_FINE=1`, `YX_PP_M=<rows>`) for real data.
- `tests/fdiv_test.cpp`: division policies against `div_ieee` bit for bit (12 operand classes, special operands), native error in ulp.
- `tests/div_ab.py`: strict against another division mode: logits per step, arg-max, state error, free-running tokens.
- `tests/pass_prof`: `YX_PP_M=<rows>` (one row count), `YX_PP_ATTN=0,1,2,..` (times every attention experiment at that context).
- Test hooks in the library: `yskn_x_set_div(mode)`, `yskn_x_set_attn(mode)`, `yskn_x_gdn_state(engine, layer, out)`.

### What a thread of a small kernel costs (measured, in-order queue)
- The engine's queue is in order: a kernel starts when the previous one is done, so with 80 to 544 threads per launch (decode) the
  time of a launch is the LATENCY of one thread, not throughput. An out-of-order queue overlaps independent launches (the same
  272-thread kernel: 3.3 us per launch whatever it computes) -- the first version of `fdiv_test` measured that and was misleading.
- Empty launch 2.5 to 3.0 us; work shorter than that is hidden in it.
- 8 rows, per launch (us): res_norm 13.8, MLP post 27.1, GDN post 27.8, conv 11.2, state 16.7, GDN out 16.5 (np_prof data).
  Work-group size (1 / 2 / 17 / 34 threads) does not change the time of the MLP work: it is not a placement effect.
- `div_ieee`: 45 to 120 ns per call in a thread (two control-register writes, each waits for all pipes: `{A@1}`); the native divide
  about 6 ns. A scalar branch to a rare fallback after a division costs 75 to 110 ns as well (measured with four formulations), so
  "native divide, check, branch to div_ieee" per division is slower than div_ieee.
- Writing 4 or 8 independent expf chains interleaved does not help while each contains a div_ieee (it serialises the pipes).

### Changes that are in the default path (bit-identical; checks below)
1. **FWHT inside a register** (`v_fwht512`, `v_fwht256`): each of the four stages pairs even and odd lanes and writes sums to the
   low half and differences to the high half; that rotates the lane index bits, so every stage is two 8-lane adds (+ 2 moves) and
   after four stages the lanes are back. Per element the same additions in the same order. Before: 2-D selects; the assembly showed
   the width-2 stage as 16 SCALAR adds per register (512 per transform) plus packing moves in the integer pipe. One transform
   1528 -> 888 instructions, about 2.1 -> 1.0 us of a thread; `k_res_norm` 5775 -> 4501 instructions, `k_mlp_post` 10225 -> 8245,
   attention `post` 7441 -> 5837, `combine` 9243 -> 7981. Two rewrites that did NOT work: the stages reordered stage-by-stage over
   the registers (same time: not latency bound), and the width-2 stage on 64-bit strided views (the compiler produced the same
   scalar adds).
2. `v_silu_n<N>` / `v_expf_n<N>` (N vectors operation by operation) in MLP post, GDN out and conv; with div_ieee inside it gains
   little in strict mode (see 3), it is what makes the native mode fast.
   Effect of 1 + 2 in strict mode: bench 83.5 -> 84.2 tok/s, 8-row pass 55.6 -> 55.0 ms, prompt reading 553 / 490 / 424 ->
   564 / 499 / 431 tok/s at 8K / 30K / 60K; quality model 77.5 -> 78.2 tok/s, 487 / 437 / 384 -> 495 / 444 / 389.

### Division modes (`YX_DIV`, `fx::g_div_mode`; res_norm, MLP post, GDN out, conv take a policy; everything else stays div_ieee)
- 0 strict (default): div_ieee. 2 native: `a / b` as compiled (inverse and product). 1 exact: `DivFast` = native divide, then the
  residuals a - q b of q0, its neighbours and one more step with fused multiply-adds (the residual is convex in q, so the candidate
  that beats both neighbours is the correctly rounded quotient), lanes outside the guaranteed range are marked and the thread
  repeats its work with div_ieee after ONE check.
- Native divide, measured on 64 M pairs per class: 1 ulp off in 29 % of the cases, 2 ulp in 0.05 %, never more for normal
  operands in these tests (not a guarantee). Special operands: 0/0, x/0, inf, NaN, subnormals agree with div_ieee in the table of
  `fdiv_test`; it differs when |b| is so large that 1/b is subnormal (3e38 / 3e38).
- `DivFast`: 0 mismatches against div_ieee on every unmarked lane (800 M pairs); 11 ns per independent division. In the real
  kernels mode 1 is nevertheless not faster than strict (8-row pass 55.8 against 55.0 ms; bench 83.3 against 84.2; tokens and state hashes identical to strict for both models): inside expf / silu
  the 22-instruction correction is a dependent chain, and how many threads repeat on real data was not measured (np_prof's probe
  showed every thread repeating, but on the scratch data described above). Kept as mode 1, not default.
- **A/B strict against native** (`tests/div_ab.py`, 200 tokens, 4 prompts: Japanese, English, code, mathematics):

  | | speed model | quality model |
  |---|---|---|
  | 8-row pass (pass_prof) | 55.0 -> 53.2 ms | |
  | pass after 8K / 30K / 60K prompt (ms) | 69.3 / 78.5 / 92.4 -> 67.9 / 76.6 / 90.7 | 71.7 / 82.1 / 95.7 -> 69.5 / 79.1 / 93.5 |
  | prompt reading 8K / 30K / 60K (tok/s) | 564 / 499 / 431 -> 581 / 512 / 440 | 495 / 444 / 389 -> 510 / 456 / 398 |
  | bench mean (different text, see below) | 84.2 -> 87.8 | 78.2 -> 80.2 |
  | bench, thinking on | 67.1 -> 64.8 | 66.1 -> 60.2 |
  | logits, same inputs: RMS error (mean over steps) | 0.056 to 0.069 | 0.055 to 0.090 |
  | logits, same inputs: largest error | 0.50 to 6.2 | 0.62 to 13.7 |
  | arg-max equal, same inputs | 199, 200, 200, 199 of 200 | 198, 196, 200, 198 of 200 |
  | DeltaNet state, relative error, layers 0 / 30 / 62 | 5e-4 / 2e-2 / 3e-2 from the first step, not growing over 200 steps | the same |
  | free-running greedy: identical tokens | 78, 200, 200, 86 of 200 | 0, 40, 200, 103 of 200 |
  | self-check | 2 of 3 (thinking differs) | 3 of 3 (strict: 2 of 3, the known near-tie) |

  A 1 to 2 ulp difference is NOT negligible in this model: every GEMM input is rounded to int8 with a scale taken from the block
  maximum and many values are rounded to bf16 / fp16, so a last-bit difference flips a rounding now and then (about 1e-5 per
  value, 17,408 values per row in the MLP), each flip is a 1/127 step, and 64 layers carry it on: 2 to 3 % relative state error in
  the deep layers and logit errors of the size of small top-1 / top-2 margins. Under the same inputs the arg-max changes in 0 to
  2 % of the positions; free-running text departs from the strict text in 5 of 8 runs (the departed texts read equally well: same
  structure and content, other wording). Mean tok/s of the bench moves by the changed text (tokens per pass), not only by the
  pass time, in both directions (thinking on is slower in native mode).
  Decision: strict stays the default (bit-exact checks against the reference, look-ahead on/off and 8/16-row equalities hold only
  there); native is an option (`YX_DIV=2`) worth 2 to 3 % of a pass and 2 to 3 % of prompt reading.

### Fused attention experiments (`YX_ATTN_FA`, `g_attn_mode`; default 0 = score16 + vacc16 untouched)
All experiments use the arithmetic of fa48 (online softmax per group of 16 keys) and are bit-identical to fa48 -- checked, not
assumed: engine state hashes and greedy continuations of `pf_check` (15 prompt lengths up to 3,000 tokens, several chunkings) are
identical to fa48 for every experiment. fa48 itself differs from the default (15 of 15).
NOT done: the same check with prompts of 9,000 and 20,011 tokens and three repeats. The first of these runs, fa48 itself
(`YX_ATTN_FA=1`, the kernel that existed before this round), ended with `UR_RESULT_ERROR_DEVICE_LOST` and a hung process; the run was
stopped, the card works again (self-check 3/3 afterwards) and the cause was not investigated. All twelve kernels did run at 8K, 30K
and 60K in `pass_prof` (timing only). One more reason to keep the fa48 family out of the default path.

Pipeline (`fa_p<QW, RG, PIPE = 1, RS>`), phase p = 0 .. R (R rounds of RG groups of 16 keys):

    time      score threads (QW)                   value threads (2 QW)
    pre       -                                    copy K(0) -> K[0]                               | barrier
    phase 0   scores(0): read K[0], write W[0],C[0]   copy K(1) -> K[1]; copy V(0) -> V[0]          | barrier
    phase p   scores(p): read K[p&1], write W[p&1],C[p&1]   copy K(p+1) -> K[(p+1)&1]; copy V(p) -> V[p&1];
                                                   values(p-1): read V[(p-1)&1], W[(p-1)&1], C[(p-1)&1]   | barrier
    phase R   -                                    values(R-1)                                     | barrier
    end       write pm, pl; l -> SLM               | barrier |  read l, write the sums

- Dependencies: the running maximum and running sum of a query live in the registers of ITS score thread and are updated in key
  order by that thread only; the sums o live in the registers of the value threads and are updated in key order (rescale by C,
  then the products of the group). The only data crossing threads are W (weights) and C (rescale factors) of a round, written in
  phase p and read in phase p + 1, and the tiles. In a phase no buffer is both read and written: K[p&1] is read, K[(p+1)&1]
  written; V[p&1] written, V[(p-1)&1] read; W / C[p&1] written, W / C[(p-1)&1] read. Every thread passes R + 2 barriers (R depends
  only on the part), against 3 R + 1 in fa48.
- SLM per work-group (zeinfo): fa48 86,016; pipeline 48 with RG 2: 87,040; RG 1: 44,032; 24 threads with RG 1 (pipeline) or RG 2
  (phases): 38,912; 24 threads pipeline RG 2 or phases RG 4: 76,800. All below 128 KiB.

256-row pass, scores + values of the 16 attention layers, ms (median of 5 after a warm-up, spread below 0.3 %), GPU exclusive:

| `YX_ATTN_FA` | kernel | 8K | 30K | 60K | vs default | threads | SLM | barriers per round | flag spill store / load | instructions |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | score16 + vacc16 | 43.8 | 152.4 | 300.3 | 1.00 | 16 + 16 | 32K + 40K | 2 + 2 | 0 / 0 | 1725 + 969 |
| 1 | fa48 | 96.9 | 347.0 | 687.8 | 2.28 | 48 | 84K | 3 | 12 / 25 | 3009 |
| 2 | pipeline 48, RG 2 | 86.9 | 310.7 | 615.5 | 2.04 | 48 | 85K | 1 | 10 / 19 | 3146 |
| 9 | pipeline 48, RG 1 | 89.1 | 318.5 | 630.9 | 2.09 | 48 | 43K | 1 | 10 / 19 | 3047 |
| 5 | pipeline 48, RG 2, vector rescale | 88.5 | 316.3 | 626.7 | 2.08 | 48 | 85K | 1 | 10 / 19 | 3108 |
| 6 | fa48 phases, vector rescale | 96.6 | 345.6 | 685.4 | 2.27 | 48 | 85K | 3 | 13 / 26 | 3029 |
| 3 | FA24 (phases), RG 2 | 103.0 | 367.1 | 725.9 | 2.41 | 24 | 38K | 3 | 10 / 22 | 3061 |
| 10 | FA24 (phases), RG 4 | 195.0 | 699.5 | 1385.7 | 4.59 | 24 | 75K | 3 | 10 / 22 | 3053 |
| 4 | pipeline 24, RG 1 | 86.6 | 309.6 | 612.9 | 2.03 | 24 | 38K | 1 | 8 / 16 | 3050 |
| 8 | pipeline 24, RG 2 | 165.9 | 595.3 | 1178.6 | 3.91 | 24 | 75K | 1 | 8 / 16 | 3130 |
| 7 | score16o + vacc16o (separate kernels, online softmax, no score buffer) | 46.4 | 163.7 | 322.0 | 1.07 | 16 + 16 | 32K + 42K | 2 + 2 | 5 / 10, 0 / 0 | 2201 + 1053 |

All have 128 GRF, no GRF spills, 24 `dpas` (16 + 8). What the numbers say:
- Running scores and values at the same time (pipeline) gains 10 % over fa48, no more: the score thread is the long pole (16 XMX
  products, 32 SLM loads and the softmax of 8 x 16 scores per group against 8 products and 17 loads in a value thread), so the
  value threads still wait about half of the time, and only 16 of the 48 threads of a work-group do score work. The default runs
  the score work on all threads of a core, then the value work on all threads.
- Half-size work-groups do not help by themselves (FA24 is slower than fa48), and with 75 to 77 KiB of SLM a 24-thread group is
  twice as slow as with 38 KiB: evidence that a second group shares the core only when two groups' SLM fit 128 KiB. Pipeline 24
  with 38 KiB equals pipeline 48.
- The vector rescale does not remove flag spills (they come from the per-query conditions of the online softmax in the score
  code: score16o has them too, score16 has none) and is 2 % slower (64 multiplies per group that were skipped before).
- Dropping the float score buffer (experiment 7) is 7 % SLOWER than the default: the stores and loads it saves (4 of about 36
  messages per thread and group) cost less than the per-query scalar work of the online softmax.
- A fused kernel in which every thread does both roles would need the query operands (64 registers) and the sums of 16 dimension
  chunks (128) in one thread: not possible with 128 GRF; with 4 queries per thread it fits but needs twice the SLM tile loads and
  XMX instructions per query. Not built.
Conclusion: score16 + vacc16 stays the default; the experiments stay in the source behind `YX_ATTN_FA` (1 to 11). Numerical error
of the fa48 family against the default and its effect on tokens were not evaluated (none is a candidate).

### ISA before / after (static counts; `tools/isa_report.py`; before = dump of the 83.5 build, after = this round)

| kernel | instructions before | after, strict | after, native | flag spills (after) | branches before / strict / native |
|---|---|---|---|---|---|
| res_norm | 5775 | 4501 | 3998 | 0 | 83 / 83 / 17 |
| MLP post | 10225 | 8245 | 6730 | 16 store, 32 load (native 8, 16) | 207 / 207 / 9 |
| GDN out | 7169 | 6494 | 4900 | 0 | 225 / 225 / 15 |
| conv | 2293 | 1781 | 1518 | 0 | 105 / 86 / 52 |
| GDN post | 5648 | 4368 | (no policy) | 0 | 25 / 25 |
| attention post | 7441 | 5837 | (no policy) | 0 | 125 / 125 |
| attention combine | 9243 | 7981 | (no policy) | 0 | 283 / 283 |

No GRF spills in any of them (128 GRF). The branches of the strict kernels are the early-out `goto` / `join` of div_ieee.

### Decode: where the 8-row pass stands (strict, 55.0 ms + head 2.2 + draft model 6.9)
GEMM 43 (floor about 40, previous section); with waits: res_norm 3.25, MLP post 2.94, GDN post 1.84, GDN state 1.5, GDN out 1.34,
conv 0.76, attention post + trellis 2.08, select 1.6, scores 1.0, combine 0.6. 90 tok/s was not reached: 84.2 strict, 87.8 native
(with different text). Next candidates by measured size: (a) the int8 tail of every a8 producer (FWHT + fp16 rounding + 33
divisions: 3.3 us x 240 launches = 0.8 ms); (b) `select` (scalar SYCL, 1.6 ms); (c) GDN post at 16 rows (2048 threads, 53 us x 48);
(d) the moves around the in-register FWHT stages (2 of the 4 instructions per stage: hand-written assembly could use the strided
regions directly -- `ocloc disasm` / `asm` exist on this machine, loading a patched binary through Level Zero was not tried);
(e) the policy for the remaining kernels (GDN post, attention post / combine, draft model) if native mode is to be used.

### Checks of the final build of this round (default modes)
Both models: `dec_check` (tokens, positions, state hashes; greedy, sampling, prefix reuse) identical to the build before this round
and identical with look-ahead off; `pf_check` states identical to the build before this round with the automatic batch and with
batch 16. Self-check: speed 3/3, quality 2/3 (the known near-tie in the thinking case). Automatic batch unchanged: speed 256 (22.19 GB
allocated), quality 128 (21.79 GB).
