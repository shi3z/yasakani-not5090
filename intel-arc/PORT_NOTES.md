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
