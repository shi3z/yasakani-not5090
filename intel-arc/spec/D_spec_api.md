# D. Generation control, draft model, sampling, public API — implementation spec

Source root: `..` (commit c0e11b9). All `file:line` refer to it.
Model folder: `models/speed`. Statements marked **UNCERTAIN** were not fully determined.

Conventions: `V=248320`, `H=5120`, `pos_` = number of committed tokens in the engine, `S` = host token sequence
(prompt + everything generated), `F = len(S)-1` = index of the *frontier* token (the newest token; it is known but has
**not** been run through the model yet). Invariant between calls: `pos_ == F`, `d_toks[0] == S[F]`.

---

## 0. What is used and what is dead code

* `yskn_open` picks the engine by `model.arch` in the `.yskn` JSON (`src/c_api.cpp:15-19`). The released model has
  `"arch": "qwen3_5"` → **only `Engine35`** (`src/engine35.cpp`) is used. `Engine` (`src/engine.cpp`, `engine.hpp`),
  `src/draft_kernels.cu/.hpp` (`launch_d3_*`, only called from `engine.cpp`) and the `Specx = SpecxT<Engine>`
  instantiation are **not used** for this model. Do not port them.
* The "MTP" drafter (`Q35Mtp`, `load_draft`, `draft_round`, `draft_catchup`, `draft_steps`, `hist_store`, `mtp_pass`,
  `mtp_log_*`, `lfrow_fill`, `engine35.cpp:1358-1510`) needs a `qwen3_5_mtp` container which is not shipped; Python
  never calls `yskn_load_draft` and `use_d3 = 0`. **Dead for the release.** Everything guarded by `mtp_.on`, `d3`,
  `d3l`, `lazy_dev`, `mtp_spot`, `wk_on`, `dump`, `fused` in `specx.cpp` is dead.
* Visual input (`yskn_set_visual`) is not bound by Python.
* The drafter that *is* used is "bdraft2" (`Engine35::Bdr`, `load_bdraft`, `bdraft_ctx`, `bdraft_step`,
  `src/q35_bdraft.cu`).
* `python/build/lib/yasakani/` is a stale copy of `python/yasakani/`; ignore.

---

## 1. The ABI used by the Python package

`python/yasakani/_native.py:25-40` binds exactly 14 symbols; **all 14 must exist** or `NativeLib` raises
("the engine library is missing exports", `_native.py:89-90`). All pointers are passed as raw integers (numpy
`arr.ctypes.data`), ints are C `int`.

| symbol | ctypes signature | called by the package? |
|---|---|---|
| `yskn_open(const char* path, int max_ctx, int device, int flags)` → `yskn_engine*` | `[c_char_p,I,I,I] -> P` | yes (`engine.py:104`, flags = 0) |
| `yskn_close(e)` | `[P] -> None` | yes |
| `yskn_reset(e)` → int | `[P] -> I` | yes |
| `yskn_prefill(e, const int32* ids, int n, int32* first)` → int | `[P,P,I,P] -> I` | **bound, never called** |
| `yskn_decode(e, int32* out, int steps)` → int | `[P,P,I] -> I` | **bound, never called** |
| `yskn_logits(e, float* out_vocab)` → int | `[P,P] -> I` | **bound, never called** |
| `yskn_position(e)` → int | `[P] -> I` | `Engine.position()`; no caller in serve/chat/bench/selfcheck |
| `yskn_nll(e, const int32* ids, int n, float* out)` → int | `[P,P,I,P] -> I` | **bound, never called** |
| `yskn_last_error(void)` → `const char*` | `[] -> c_char_p` | yes |
| `yskn_cancel(e)` → int | `[P] -> I` | yes (other thread) |
| `yskn_set_opt(e, int key, int value)` → int | `[P,I,I] -> I` | yes (keys 10,11,12,13,16,29 only) |
| `yskn_load_bdraft(e, const char* path)` → int | `[P,c_char_p] -> I` | yes, once right after open |
| `yskn_spec_generate(e, prompt, n_prompt, n_gen, cfg, n_cfg, out_ids, stats, times_ms)` → int | `[P,P,I,I,P,I,P,P,P] -> I` | yes (the only generation entry) |
| `yskn_state_hash(e, uint64* out, int max_out, int* n_out)` → int | `[P,P,I,P] -> I` | `Engine.state_hash()`; no caller in the CLI |

`yskn_cancel`, `yskn_load_bdraft`, `yskn_state_hash` are **not** declared in `include/yskn_abi.h`; they are defined in
`src/c_api.cpp:78,117,112` with `extern "C" YSKN_API`.

### 1.1 Error reporting and return codes

* Every `int` function: `0` = ok, `-1` = a C++ exception was caught; the message is stored in a
  `static thread_local std::string g_err` (`c_api.cpp:11,14`). `yskn_last_error()` returns `g_err.c_str()` of the
  **calling thread** (`c_api.cpp:27`). It is not cleared on success.
* `yskn_open` returns `NULL` on failure (message in `g_err`). Python appends a hint when the message contains the
  substring `"memory"` (case-insensitive, `engine.py:108-109`) → make out-of-memory messages contain "memory".
* `yskn_position` returns `pos_` directly, never fails. `yskn_cancel` always returns 0. `yskn_close(NULL)` is a no-op.
* Python reads `last_error()` **on the thread that made the failing call** (for generation that is the worker
  thread, `engine.py:229-231`).

### 1.2 Threading expectations

* `Engine.generate` starts a **new Python thread per request** and calls `yskn_spec_generate` on it
  (`engine.py:225-236`); `yskn_open`, `yskn_load_bdraft`, `yskn_set_opt`, `yskn_reset` run on the main thread or an
  HTTP handler thread. So the library is entered from many different OS threads over its life, but never
  concurrently — **except `yskn_cancel`**, which is called from the polling thread while `yskn_spec_generate` runs
  (`engine.py:159-163,252,257`; `chat.py:258,273,328`; `cli.py:230`). Do not keep per-thread engine state other than
  the error string.
* `yskn_cancel` = `e->cancel.store(1)` on a `std::atomic<int>` inside the handle (`c_api.cpp:12,78`).
  `yskn_spec_generate` stores 0 into it on entry (`c_api.cpp:40`) and the loop tests it once per pass
  (`specx.cpp:203`). A cancel that arrives before entry is lost (Python serialises, fine). A cancelled call returns
  **0** with `stats.eos = 0` and `produced < n_gen`; the engine state stays valid and continuable.
* Python serialises everything else with an `RLock`; the server additionally holds `gen_lock` (`server.py:293`).

### 1.3 Per-function semantics

**`yskn_open(path, max_ctx, device, flags)`** (`c_api.cpp:15-19`, ctor `engine35.cpp:184-373`).
`cudaSetDevice(device)`; `max_ctx` is rounded up to a multiple of 64 (`engine35.cpp:219`). Python passes
`pack.max_ctx` (262144 from `yasakani.json`) unless `--max-ctx` is given. Environment variables are read **here**
(Python sets them before the call, `runtime.py:27-43`, and *removes every other `YSKN_*` variable from the
process environment*, `runtime.py:93-101` → **port-specific knobs must not be named `YSKN_*`**).
Initial sampling state: `smp_ = 0` (greedy), `smp_p_ = {temp 0.6, top_p 0.95, top_k 20, version 3, seed 0, pm 0, min_p 0}`
(`engine35.hpp:312`, `engine35.cpp:345`).

**`yskn_load_bdraft(e, path)`** (`engine35.cpp:1611-1697`). Opens the drafter container (must be arch `"bdraft2"`),
allocates drafter buffers, calls `set_capture({5,19,33,47,61})` (`:1650`), loads the SHEAD head from
`$YSKN_BDR_SHEAD` with `M = $YSKN_BDR_SHEAD_M` (512) (`:1664-1688`), uploads the HCAP tables and sets
`hb_on = 1` unless `$YSKN_HCAP` says otherwise (`:1689-1693`; Python does not set it → 1). Python always calls it;
the default cfg has `bdraft=1`, and `specx.cpp:173` throws "bdraft needs Engine35::load_bdraft" if it was not loaded.

**`yskn_reset(e)`** (`engine35.cpp:392-401`): waits for a pending replay, drops the prompt snapshot
(`snap_drop`), bumps `sh_epoch_` (invalidates the "live" continuation), `pos_ = 0`, zeroes every DeltaNet state `S`
(48×128×128 f32 per linear layer) and conv history (3×10240 bf16). KV rows and drafter rings are **not** cleared (they
are overwritten / masked by position).

**`yskn_position(e)`**: `pos_`.

**`yskn_set_opt(e, key, value)`** (`engine35.cpp:908-962`). First waits for any pending state replay. Keys the Python
side sets (`engine.py:20,166-178`), always in this order on every request:
`11 (top_p) → 12 (top_k) → 13 (seed) → 10 (temperature) → 16 (sampler version) → 29 (min_p)`.

| key | name | value | effect | validation |
|---|---|---|---|---|
| 10 | temperature | `round(T*1000)` | `smp_ = value > 0`; if on, `temp = value/1000.0`. `0` = greedy (temp untouched). | none |
| 11 | top_p | `round(p*1000)` | `top_p = value/1000.0` | must be 1..1000 else error "set_opt: top-p x 1000 must be 1..1000" |
| 12 | top_k | int | `top_k = value` | must be 1..20 (`Q35_SMP_K`) |
| 13 | seed | int | `seed = (uint64)(uint32)value` (only 32 bits survive) | none |
| 16 | sampler version | 2 or 3 | `version`; Python always sends 3 (Gumbel-max) | else error |
| 29 | min_p | `round(min_p*1000)` | `min_p = value/1000.0` | 0..1000 |

Python normalises first (`engine.py:36-42`): `temperature <= 0` → `(T=0, top_p=.95, top_k=20, min_p=0, seed=0)`;
`top_k` outside 1..20 → 20. Any other key the port does not implement should throw
`"set_opt: unknown key N"` (as `:961`). Other keys exist (1,2,3,4,5,6,7,8,9,14,15,20..28,30) but only `specx.cpp`
uses 4 and 6 internally (`e_.set_opt(6,1)` = enable row tables for the HCAP pass, `specx.cpp:195`).

**`yskn_spec_generate`** — see 1.4 and section 2.

**`yskn_cancel(e)`** — see 1.2.

**`yskn_state_hash(e, out, max_out, n_out)`** (`engine35.cpp:1585-1609`). Fills `out[0] = pos_`, then per layer in
order: linear layer → FNV(S), FNV(conv hist); attention layer → FNV(K rows 0..pos-1 of the 4 KV heads),
FNV(V rows), and with 8-bit/KV5 storage FNV(K scales), FNV(V scales). `*n_out` = count (1 + 48·2 + 16·4 = 161).
Throws if `max_out` too small (Python gives 512). FNV variant: `h ^= word64; h *= 0x100000001b3`, start
`0xcbf29ce484222325`, per 8-byte word then per trailing byte (`:1540-1545`). Only used to compare two runs of the
*same* library; values need not equal CUDA's.

**Never-called but required symbols** (implement simply or return -1 with a message):
* `yskn_prefill(e, ids, n, &first)` (`engine35.cpp:982-987,1200-1217`): appends n tokens at `pos_`
  (`pos_+n <= max_ctx` else "prefill: context overflow"), `*first` = next token (argmax, or the sample when
  sampling is on), leaves `d_toks[0] = first`.
* `yskn_decode(e, out, steps)` (`:1268-1284`): `steps` times: run the 1-row committing pass on `d_toks[0]` at
  `pos_`, `pos_++`, `out[i]` = the token it predicts, which becomes `d_toks[0]`. (So `prefill` returns token #0 and
  `decode` returns tokens #1..)
* `yskn_logits(e, out)`: `V` floats, row 0 of the last head slice. `yskn_nll(e, ids, n, out)`: teacher-forced NLL
  of `ids[i+1]` for i in 0..n-2, commits n-1 tokens.

### 1.4 `yskn_spec_generate(e, prompt, n_prompt, n_gen, cfg, n_cfg, out_ids, stats, times_ms)`

`c_api.cpp:37-75`. Sequence:

```
if n_cfg < 12: error "spec_generate: need at least 12 config values"
e->cancel = 0
build SpecxConfig from cfg
Specx35 y(engine, cfg, V)                 # allocates the V x 8 next-token table etc.; throws if gamma not in 0..15
t0; out_ids[0] = y.prefill(prompt, n_prompt); t1
y.generate(n_gen, out_ids + 1); t2
times_ms[0] = t1-t0 (ms); times_ms[1] = t2-t1 (ms)
fill stats
if cfg.snap: engine.snap_note(S[0 .. len(S)-2])     # remember what the live state holds (c_api.cpp:71)
return 0
```

**Buffers** (`engine.py:214-216`): `out_ids` = int32[`n_gen+1`], pre-filled with **-1** by Python; `stats` =
double[48] zeroed; `times_ms` = double[2]. `n_gen = max(0, min(max_new-1, max_ctx - n_prompt - 64))`
(`engine.py:181-188`). `n_gen` may be 0.

**Streaming is polling, not a callback.** While the worker thread is inside the call, the calling thread scans
`out_ids` every `poll_s = 1 ms` for the first index still `< 0` and hands `out[:n]` to `on_tokens`
(`engine.py:240-255`). Therefore the engine **must write `out_ids[k]` in increasing k, as soon as each token is
committed**, and must never write a negative value. `out_ids[0]` is written right after prefill. After return Python
takes `ids = out_ids[: min(stats[30] + 1, n_gen + 1)]` (`engine.py:268-270`).

**Stop conditions** (`specx.cpp:166-168,203,412-413,440`):
* `produced >= n_gen` → finish "length" (Python: `eos == 0` and not cancelled).
* a stop id (`eos1`/`eos2`) is emitted → it **is** written as the last id; `stats.eos = 1`. If the prompt-pass token
  (`out_ids[0]`) is already a stop id, the loop does not run (`produced = 0`, `eos = 1`).
  Stop ids come only from cfg[20], cfg[21] (Python: `EOS_IDS = (248046, 248044)`; `ignore_eos` → both `0x7FFFFFFF`).
  `is_eos(t) = t >= 0 && (t == eos1 || t == eos2)`.
* cancel flag set → checked before each pass.
* Context: `room = min(n_gen - produced - 1, max_ctx - pos_ - 1)` bounds the draft length each pass; Python
  guarantees `n_prompt + n_gen + 64 <= max_ctx`.

**Sampling parameters are not in the call**: they are engine state set by `yskn_set_opt` before it.

**cfg (int32[44], `runtime.py:47-59`, parsed at `c_api.cpp:41-54`)** — default values of the Python package in
brackets. "dead" = no effect in the default configuration.

| i | C field (Python name) | default | meaning |
|---|---|---|---|
| 0 | gamma | 7 | max draft tokens per pass on the short path; must be 0..15 |
| 1 | use_d0 | 1 | "copy from the input" drafts (n-gram match in `S`) |
| 2 | use_d1 | 1 | next-token-table drafts |
| 3,4 | d0_nmin, d0_nmax | 2, 8 | n-gram lengths tried for D0 (longest first) |
| 5 | d0_priority_n | 3 | non-drafter path only: D0 match ≥ this beats D1 |
| 6 | extend | 1 | after a D0 chain, extend it with D1 up to the row budget |
| 7 | d1_rejected | 1 | also record top-8 of rejected rows into the tables |
| 8 | auto_gamma | 0 | dead |
| 9 | use_graph | 1 | CUDA graphs; **ignore in the port** |
| 10,11 | use_d3, d3_steps | 0,0 | MTP drafter; dead. `bdraft=1` with `use_d3=1` is an error |
| 12 | d1_bigram | 1 | keep/use bigram table T2 |
| 13 | d1_static | 0 | static table (needs `yskn_specx_load_static`); dead |
| 14,15,16 | d3_extend, d3_lazy, d3_gate | 0 | dead |
| 17 | gamma_long | 15 | row budget when a long D0 match exists |
| 18 | long_n | 6 | `full_prev && n0 >= long_n` → long budget |
| 19 | d1_trigram | 1 | keep/use trigram table T3 |
| 20,21 | eos1, eos2 | 248046, 248044 | stop ids |
| 22 | long_d0 | 8 | `n0 >= long_d0` → long budget |
| 23 | sel | 0 | cost model; dead (needs cost_tok_us > 0) |
| 24,25 | cost_tok_us, cost_row_us | 0 | dead |
| 26,27 | cost_mtp_us (cost_nd_us), mtp_auto (nd_auto) | 0 | dead |
| 28 | prmode | 0 | dead |
| 29 | cost_row2_us | 0 | dead |
| 30 | lfrow | 0 | host-built leaf rows for MTP; dead |
| 31 | wk_q_pm (lfrow_q_pm) | 300 | dead |
| 32 | mark_tok | -1 | if ≥0: stats[35],[36] = time/index when this id was first emitted |
| 33,34,35 | cost_tok_k, cost_row2_k, cost_mtp_k | 0 | dead |
| 36 | bdraft | 1 | use the bdraft2 drafter |
| 37 | bdr_long_n | 4 | D0 match ≥ this → D0 competes with the drafter |
| 38 | bdr_adapt | 1 | 1 = pick D0 vs drafter by running average; 2 = always D0 when long |
| 39 | bdr_probe | 4 | every 4th D0-pass also run the drafter to keep its average fresh |
| 40 | bdr_d0_len | 15 | max D0 chain length on the drafter path |
| 41 | prefill_head | 1 | run the head on **every prompt row** and seed the tables with their top-8 |
| 42 | snap | 1 | prefix reuse (live continuation + prompt snapshot) |
| 43 | snap_tok | 248045 (`<|im_start|>`) | snapshot is taken right after its last occurrence |

Per-request overrides come through `Engine.generate(spec=...)`; `prefill_head`, `snap`, `snap_tok` cannot be
overridden per request (`runtime.py:63`). `quickstart/lookahead_check.py:22` uses
`NO_LOOKAHEAD = dict(bdraft=0, use_d0=0, use_d1=0)` — **this is the "no look-ahead" switch** (one row per pass).

**stats (double[], `c_api.cpp:60-68`)** — Python reads only the bold ones (`runtime.py:66`).

| i | value |
|---|---|
| **0** | **passes** (main-model passes in the generate loop; prefill not counted) |
| **1** | **tokens** emitted by those passes (= produced) |
| 2 | chain_none: passes with no draft |
| 3,4 | drafted / accepted tokens of source 0 (D0 copy) |
| 5,6 | drafted / accepted of source 1 (D1 table) |
| 7..22 | rows_hist[M] for M = 1..16 (index 6+M): passes that verified M rows |
| 23,24 | drafted / accepted of source 2 (drafter) |
| 25,26,27 | ms_verify, ms_draft, ms_total (wall, host side) |
| 28,29 | ms_commit, ms_record |
| **30** | **produced** = `min(produced, n_gen)` = ids written to `out_ids[1..]` |
| **31** | **eos**: 1 if a stop id ended it, else 0 |
| 32,33,34 | leaves, leaf_breaks, leaf_hits (side-candidate rows) |
| 35,36 | ms_mark, mark_at (-1 if unused) |
| **37** | bdr_rounds (passes drafted by the drafter) |
| **38** | bdr_d0 (passes where D0 replaced the drafter) |
| 39,40 | bdr_probes, ms_ctx |
| **41** | **sh_mode** ("snap_mode"): 0 fresh prefill, 1 continued the live state, 2 restored the snapshot |
| **42** | **sh_start** ("cached_tokens"): prompt ids not recomputed |

`times_ms[0]` = prefill wall ms, `times_ms[1]` = generate wall ms. Derived by Python: `completion_tokens =
produced + 1`; bench speed = `completion_tokens / (gen_ms/1000)`, `tokens_per_pass = completion_tokens /
max(1, passes)` (`cli.py:381-383`); `finish = "stop" if eos==1 else ("cancelled" if Python called cancel else
"length")`.

### 1.5 Call sequences of the CLI

Common start (`cli.py:102-125`, `engine.py:90-117`): set env → `yskn_open(weights, max_ctx, device, 0)` →
`yskn_load_bdraft(draft)` → **warm-up** (`engine.py:279-286`, `chat.py:199-202`; skipped only by `serve --no-warmup`):

```
set_sampling(T=1.0, top_p=.95, top_k=20, min_p=0, seed=999, mode=3)   # 6 x yskn_set_opt  -> SAMPLING ON
for p in [encode("warm-up"), chat_prompt("warm-up")]:
    yskn_reset; yskn_spec_generate(p, n_gen=32, default cfg)           # results ignored
yskn_reset
```

* **selfcheck** (`cli.py:418-430`): `set_sampling(T=0)` → set_opt (11,950),(12,20),(13,0),(10,0),(16,3),(29,0); then
  for each of 3 cases: `yskn_reset`; `spec_generate(prompt, max_new-1, default cfg, stops = EOS_IDS)`; final
  `yskn_reset`. PASS = for every case `sha256(int32 LE ids)` and the id count equal `selfcheck.json`
  (prompt 19 → 3 ids `[9419, 0, 248046]`; prompt 69 → 160 ids; prompt 5 → 32 ids). The reference was produced by
  the CUDA engine, so **every generated id must equal the CUDA engine's** (argmax-identical, greedy).
* **bench** (`cli.py:367-396`): per prompt/run: `yskn_reset`; `set_sampling(T=0, …, seed = next counter)`;
  `spec_generate(prompt, n-1, default cfg with eos1=eos2=0x7FFFFFFF)`. Uses `gen_ms`, `passes`, id count.
* **chat / serve** (`chat.py:227-306`): per request `set_sampling(T, top_p, top_k, min_p, seed)` (seed = request seed or
  a counter from `--seed0`), then `spec_generate`. **No reset between requests** → prefix reuse matters. Possible
  second `spec_generate` on `prompt + output + bridge` after a loop-guard cancel (`chat.py:308-339`).
  `yskn_reset` only after an engine error (`cli.py:316`).
* `lookahead_check.py`: `reset; set_sampling(greedy); generate(ids, n, spec=NO_LOOKAHEAD or None)` and compares ids.

---

## 2. The generation loop (default configuration)

Code: `src/specx.cpp` (`SpecxT<Engine35>`), engine primitives in `src/engine35.cpp:843-1539`.

### 2.1 Engine primitives used

```
prefill_rows(ids, n, am[], top8_ids[], top8_vals[]) -> next        engine35.cpp:1244-1267
    for chunks of M = min(PM_, remaining):      # PM_ = Pmax_ = $YSKN_Q35_PMAX = 1024 (engine35.cpp:194-197)
        d_toks[0..M) = ids chunk; d_pos = pos_
        run_pass(M, commit=true, HEAD_ALL, topk=true)        # all M rows, state+KV committed, head on every row
        if drafter loaded: bdraft_ctx(row0=0, n=M, pos0=pos_)  # drafter context rows from the captured hiddens
        read d_next[0..M) (-> am), top-8 ids/values per row
        pos_ += M
    next = d_next[last row]; d_toks[0] = next; d_pos = pos_

verify(ids[M], M, ..., am[M], top8[M][8])                          engine35.cpp:1299-1318   (host-supplied rows)
verify_device(M, ..., am[M], dev_ids[M], top8[M][8])               engine35.cpp:1519-1539   (rows already in d_toks)
    require 1 <= M <= 16 (HM_), pos_+M <= max_ctx
    c1 = (M == 1 && specx_scope)        # opt_single_commit_=1: a 1-row pass commits itself
    run_pass(M, commit=c1, HEAD_ALL, topk=true); pre_committed = c1; last_M = M
    am[r]   = d_next[r]  = token the main model chooses for position pos_+r+1 given rows 0..r
              (argmax; or the *sampled* token when sampling is on — the sampler overwrites d_next)
    top8[r] = 8 largest logits of row r (ids, values), sorted by (value desc, id asc)

commit(n_accept)                                                   engine35.cpp:1319-1330
    require 0 <= n_accept < last_M
    if pre_committed: require n_accept == 0
    else replay_commit(n_accept + 1)      # advance the DeltaNet states by rows 0..n_accept
    pos_ += n_accept + 1
    d_pos = pos_; d_toks[0] = d_next[n_accept]      # the new frontier token is already on the device
    last_M = 0

set_verify_tokens(ids, first_row, n): d_toks[first_row .. +n) = ids      engine35.cpp:1511-1515
bdraft_step(n_ctx, pos_ctx, draft, use_graph)                            engine35.cpp:1838-1860  (section 3.6)
bdraft_out(int32 drafts[15]): copy the drafter's last chain to the host  engine35.cpp:1868-1871
```

What a **non-committing verify pass** (M rows, `n_commit = 0`) does to engine state:
* Row r is token `d_toks[r]` at absolute position `pos_ + r`; row 0 is the frontier token.
* Attention layers: K/V of row r are written into the cache at slot `pos_ + r` (provisional; the next pass
  overwrites slots ≥ the new `pos_`). Row r attends cache positions `0 .. pos_+r`.
* Linear (Gated DeltaNet) layers: the state `S` and the 3-row conv history are **read, not written**; the M rows are
  stepped through sequentially inside the kernel from the committed state. Per-row intermediates are kept in
  per-layer buffers: conv output `q,k,v,beta,g` → `Y.tq, Y.tk, Y.tv, Y.tbeta, Y.tg` (`opt_kconv_`, when M ≤ 16,
  `engine35.cpp:785-786`) and the pre-conv mixed rows `Y.pmix`.
* The hidden state after layers 5, 19, 33, 47, 61 is copied for all M rows into `cap_[k][row]`
  (`engine35.cpp:832-833`) — the drafter's input features.
* Head: logits for all M rows → `d_next[r]`, top-8.

**`replay_commit(rows)`** (`engine35.cpp:843-866`): for every linear layer
`launch_q35_state(Y.tq, Y.tk, Y.tv, Y.tbeta, Y.tg, Y.S, M=rows, n_commit=rows, core_(scratch), Y.pmix, Y.hist)`:
re-runs the state recurrence over rows `0..rows-1` from the kept intermediates and now **stores** `S` after the last
row, and shifts the conv history: `hist[k] = (rows-3+k >= 0) ? pmix[rows-3+k] : old_hist[k + rows]`, k = 0..2
(the last 3 mixed rows; see `lfrow_kernels.cuh:93-101` for the same code). The attention layers need nothing (their
K/V rows are already in place). In CUDA this runs asynchronously on a second stream, overlapped with the next draft,
and `replay_fence()` joins it before the next main pass / set_opt / reset / snapshot. **Port: run it synchronously
first; overlapping is an optimisation.** The result must be bit-identical to having run those rows in a committing
pass, otherwise text depends on speculation.

**A committing pass** (`commit=true`, prefill and the 1-row pass) passes `n_commit = M` to the same kernels, which
then store `S`/history themselves.

### 2.2 Prefill and prefix reuse (`SpecxT::prefill`, `specx.cpp:94-127`)

```
prefill(ids[0..n)):
    start = 0; P = -1; mode = 0
    if cfg.snap:
        ext(v) := v non-empty and len(v) < n and v == ids[0..len(v))        # strict prefix
        if engine.snap_live_ok() and ext(engine.snap_live()):               # mode 1: continue the live state
            start = len(snap_live); mode = 1
        elif engine.snap_pos() > 0 and ext(engine.snap_ids()):              # mode 2: restore the snapshot
            engine.snap_load(); start = snap_pos; mode = 2
        else:
            engine.reset()                                                  # mode 0 (also drops the snapshot)
        q = -1
        if cfg.snap_tok >= 0:
            for p = n-1 down to start: if ids[p] == snap_tok: q = p+1; break
        if q < 0: q = n-1
        if q > start and q < n: P = q
    # (cfg.snap == 0: NO reset is done here; the caller must have reset)
    run(a, b) := prefill_rows(ids+a, b-a, am+a, ti+8a, tv+8a)     # prefill_head=1; else engine.prefill()
    if P > 0:
        run(start, P)
        engine.snap_save(ids, P)                 # snapshot of the state after ids[0..P)
        first = run(P, n)
    else:
        first = run(start, n)
    S = ids[0..n) + [first]
    clear T2, T3 (bigram/trigram); the unigram table T was freshly allocated (all -1) in the constructor
    if prefill_head: for p in start..n-1: record(ids[p-2] or -1, ids[p-1] or -1, ids[p], top8 ids of row p, top8 vals)
    reset the D0 index, stats; stats.sh_mode = mode; stats.sh_start = start
    return first
```

Engine side (`engine35.hpp:126-133`, `engine35.cpp:1550-1584`):
* **Live continuation.** After every `spec_generate` with `snap`, `snap_note(S[0..len-2])` stores the ids the
  engine state now covers (`sh_cur_`) and `sh_note_ = sh_epoch_`. `snap_live_ok() := sh_note_ == sh_epoch_ &&
  len(sh_cur_) == pos_ && pos_ > 0`. `sh_epoch_` is incremented by every state-changing call (`run_pass`, `commit`,
  `reset`, `decode`, `snap_load`, `bdraft_ctx`, `bdraft_step`, `prefill_vsp`); `set_opt` does not change it.
  So mode 1 applies when the new prompt strictly extends "old prompt + everything generated except the last id".
* **Snapshot** `snap_save(ids, n)` (requires `n == pos_`): copies to host memory, for each of the 48 linear layers,
  `S` (48·128·128 f32 = 3.15 MB) and the conv history (3·10240 bf16), plus the 10 drafter rings
  (5 layers × K,V; each 8·2048·128 bf16). Sets `sh_pos_ = n`, `sh_ids_ = ids[0..n)`. The KV cache is **not** copied:
  rows `< sh_pos_` stay valid on the device because nothing rewrites them until a `reset()` (which drops the
  snapshot). `snap_load()` copies those buffers back and sets `pos_ = sh_pos_`.
  Total ≈ 151 MB + 42 MB of host memory, allocated on first use.
* Where the snapshot lands: right after the last `<|im_start|>` in `ids[start..n)` (the head of the final assistant
  turn — everything before it is a prefix of the next request of the same conversation); with no such token (raw
  text) at `n-1`. Consequence: **the prefill is normally split into two passes** `[start,P)` and `[P,n)`; with a
  1-token prompt there is no split.

Port note: prefill chunking (1024 rows, head on all rows) is a speed matter. Any row batching is acceptable **iff**
it yields the same ids as CUDA; `prefill_head` only feeds the D1 tables (speed), so computing the head for the last
row only and skipping `record` changes no output. `prefill_vsp` / `YSKN_Q35_LAYER_ROWS` (`engine35.cpp:1097-1176`)
is reachable only through `yskn_prefill` when no cfg is involved (`prefill_head=0`), i.e. **not** from Python.

### 2.3 Host draft sources (always available, cheap)

`T[V][8]` (int32, -1 = empty): last recorded top-8 next ids after token `cur`; `T2[(prev,cur)] → id`,
`T3[(p2,prev,cur)] → id` (hash maps, keys `(a*V+b)*V+c`, `specx.hpp:128-129`).
`record(p2, prev, cur, top8)`: `T[cur] = top8; if d1_bigram && prev>=0: T2[(prev,cur)] = top8[0];
if d1_trigram && p2>=0 && prev>=0: T3[(p2,prev,cur)] = top8[0]` (`specx.cpp:89-93`). Note `top8[0]` is the
arg-max, also when sampling.
`next_tok(p2, prev, cur)` = T3 hit, else T2 hit, else `T[cur][0]` if ≥ 0, else -1 (`specx.cpp:60-69`).

**D0 (copy)** `d0_lookup(F, chain, gmax)` (`specx.cpp:37-59`): an index `ng[n][hash(S[e-n+1..e])] = e` for
n = d0_nmin..d0_nmax is maintained for all end positions `e < F` (later occurrences overwrite). For n = 8 down to 2:
if the n-gram ending at F occurred before ending at `p` (verified token by token), `chain = S[p+1 .. min(F, p+gmax)]`
and the function returns `n`. Returns 0 with an empty chain otherwise. Hash:
`h = 0x9E3779B97F4A7C15 ^ n; for each token t: h ^= t + 0x9E3779B97F4A7C15 + (h<<6) + (h>>2); h *= 0xBF58476D1CE4E5B9`.

### 2.4 The loop (`SpecxT::generate`, `specx.cpp:128-468`), reduced to the live paths

State per call: `dema[2] = {3.0, 3.0}` (running accepted length: [0] drafter, [1] D0), pending comparisons `dpend`,
`dprobe = 0`, `dctx_n = 0, dctx_pos = 0` (context rows owed to the drafter), `full_prev = false`,
block-16 controller `b16_ema = 0, b8_ema = 4, b16_since = 0, full8_ema = 0, b16_n = 0, b16_probe_cur = 256`.
Constants (env-tunable in CUDA, defaults): `blk_mode = 2, b16_ratio = 0.22, b16_probe = 256, b16_decay = 0.9,
b16_max = 256, b16_full = 0.45, b16_floor = 0.6, b16_decay8 = 0.97, b16_full_since = 4` (`specx.cpp:142-154`).
`hb = drafter loaded && hcap_on()`; if `hb`: `set_opt(6,1)` for the duration (row tables on), restored on exit.
`engine.set_specx_scope(true)` for the duration (1-row passes commit themselves).

```
produced = 0; stop = is_eos(S[F])
while produced < n_gen and not stop and not cancelled:
    F = len(S)-1; xF = S[F]; xP = S[F-1] or -1; xQ = S[F-2] or -1
    room    = min(n_gen - produced - 1, max_ctx - pos_ - 1)
    g_short = max(0, min(gamma, room))                    # gamma = 7
    g_all   = max(0, min(max(gamma, gamma_long), room))   # 15
    n0, c0  = d0_lookup(F, g_all) if g_all > 0 else (0, [])
    long    = gamma_long > gamma and ((full_prev and n0 >= long_n) or (long_d0 > 0 and n0 >= long_d0))
    g_eff   = min(long ? g_all : g_short, max(15, bdr_d0_len))
    c0      = c0[0 .. g_eff)
    chain = []; src = []; on_device = bdr_dev = bdr_longm = bdr_d0 = hb_pass = false; nleaf = 0

    # --- how many rows: 8-row block (7 drafts) or 16-row block (15 drafts) ---
    kdfl = min(7, g_eff); b16_pass = false; room16 = min(15, room)
    if room16 > 7 and ( b16_ema >= max(b16_floor, b16_ratio*b8_ema)
                        or b16_since >= b16_probe_cur
                        or (full8_ema >= b16_full and b16_since >= b16_full_since) ):
        kdfl = room16; b16_pass = true

    if g_eff > 0:
        bdr_longm = (n0 >= bdr_long_n) and c0 not empty
        bdr_d0    = bdr_longm and (bdr_adapt == 2 or (bdr_adapt == 1 and dema[1] > dema[0]))
        probe     = bdr_d0 and bdr_adapt == 1 and bdr_probe > 0 and (++dprobe % bdr_probe == 0)
        run       = (not bdr_d0) or probe
        if probe and room16 > 7: kdfl = room16; b16_pass = true
        hb_pass = hb and not bdr_d0 and not b16_pass and kdfl == 7 and pos_ + 8 <= max_ctx
        if hb: engine.hcap_pass(hb_pass)
        if run: engine.set_verify_tokens([xF], 0, 1)
        engine.bdraft_step(dctx_n, dctx_pos, draft = run ? (b16_pass ? 2 : 1) : 0); dctx_n = 0
              # 1) feed the dctx_n rows committed by the previous pass to the drafter (catch-up)
              # 2) draft: writes 7 (or 15) tokens to d_toks[1..]; with hb_pass may rewrite rows 5..7 (section 3.5)
        if bdr_d0:
            c0 = c0[0 .. bdr_d0_len); chain = c0; src = [0]*len
            if extend and use_d1 and chain and len(chain) < g_eff:
                while len(chain) < g_eff: t = next_tok(last 3 of S+chain); if t < 0: break; chain += [t]; src += [1]
            stats.bdr_d0++
            if probe: dr = engine.bdraft_out(); dpend += {F, which=0, ch=dr[0..kdfl)}; stats.bdr_probes++
        else:
            chain = [?]*kdfl; src = [2]*kdfl; on_device = bdr_dev = true; stats.bdr_rounds++
            if bdr_longm: dpend += {F, which=1, ch=c0[0 .. min(len(c0), bdr_d0_len))}
    elif dctx_n:                                           # no draft this pass, still feed the context
        engine.bdraft_step(dctx_n, dctx_pos, 0); dctx_n = 0

    kch = len(chain); M = kch + 1 + nleaf                  # rows: frontier + drafts (+ leaves)

    # --- verify: one main-model pass over M rows ---
    if bdr_dev:
        if hb_pass: engine.lfrow_device_table()            # keep the row table the drafter wrote
        engine.verify_device(M) -> am[M], dev_ids[M], ti[M][8]
        if hb_pass:
            cf = engine.hcap_cfg()                         # {L, nleaf, depth[0..2], ...}
            require 4 <= L <= 7 and cf[1] == 7 - L
            chain.resize(L); kch = L; nleaf = 7 - L
            leaf_row[i] = L+1+i; leaf_depth[i] = cf[2+i]
        require dev_ids[0] == xF
        chain[j-1] = dev_ids[j] for j = 1..kch;  ids = dev_ids[0..M)
    else:
        ids = [xF] + chain; engine.verify(ids, M) -> am, ti

    # --- acceptance ---
    a = 0; while a < kch and chain[a] == am[a]: a++        # longest prefix the main model agrees with
    if bdr_dev:                                            # 8/16-row controller
        b8_ema = b16_decay*b8_ema + (1-b16_decay)*(min(a,7)+1)
        if b16_pass: b16_observe(a)
        else: b16_since++; full8_ema = 0.75*full8_ema + 0.25*(a >= 7); b16_ema *= b16_decay8
    a_all = a
    if bdr_longm: w = bdr_d0 ? 1 : 0; dema[w] += (a_all - dema[w]) / 16
    for j in 0..a_all-1: if is_eos(chain[j]): a = j; stop = true; break
    if not stop and is_eos(am[a]): stop = true

    hit_row = -1                                           # side candidates (only when nleaf > 0)
    if nleaf and not stop and a == a_all and a < kch:
        for i in leaves with leaf_depth[i] == a+1: if hit_row < 0 and dev_ids[leaf_row[i]] == am[a]: hit_row = leaf_row[i]
        (stats.leaf_breaks / leaf_hits)
    if hit_row >= 0: engine.lfrow_promote(hit_row, a+1)    # move that row's data to row a+1
    n_commit = (hit_row >= 0) ? a+1 : a

    # --- learn (host tables) ---
    in_tok(j) = ids[j] if j >= 0 else (xP if j == -1 else xQ)
    if d1_rejected: for j in a+1..kch: record(in_tok(j-2), in_tok(j-1), ids[j], ti[j])
    for j in 0..a: record(in_tok(j-2), in_tok(j-1), ids[j], ti[j])
    if hit_row >= 0: record(in_tok(a-1), in_tok(a), am[a], ti[hit_row])
    full_prev = kch > 0 and a == kch

    # --- commit ---
    engine.commit(n_commit)                                # rows 0..n_commit; pos_ += n_commit+1
    dctx_n = n_commit + 1; dctx_pos = F                    # their captured hiddens go to the drafter next step
    for j in 0..a-1: S += [chain[j]]; out[produced++] = chain[j]
    S += [am[a]]; out[produced++] = am[a]                  # at the first disagreement the main model's token is taken
    if hit_row >= 0: S += [am[hit_row]]; out[produced++] = am[hit_row]; if is_eos(am[hit_row]): stop = true

    # --- resolve pending "what would the other source have scored" ---
    for p in dpend:
        k = number of leading p.ch[i] == S[p.F+1+i] (stop at mismatch or when S is not long enough yet)
        if mismatch found or k == len(p.ch):
            dema[p.which] += (k - dema[p.which]) / 16
            if p.which == 0 and len(p.ch) > 7: b8_ema = b16_decay*b8_ema + (1-b16_decay)*(min(k,7)+1); b16_observe(k)
            remove p
    stats.passes++; stats.tokens += emitted; stats.rows_hist[M]++; if kch == 0: stats.chain_none++
    for k in 0..kch-1: stats.drafted[src[k]]++; if k < a: stats.accepted[src[k]]++

if dctx_n: engine.bdraft_step(dctx_n, dctx_pos, 0)         # flush: the drafter rings are complete for the next call
stats.produced = min(produced, n_gen); stats.eos = stop

b16_observe(a):  v = max(0, a-7); b16_ema = (b16_n++ == 0) ? v : b16_decay*b16_ema + (1-b16_decay)*v; b16_since = 0
                 b16_probe_cur = (b16_ema >= max(b16_floor, b16_ratio*b8_ema)) ? b16_probe : min(b16_max, 2*b16_probe_cur)
```

(`out[...]` writes are guarded by `produced < n_gen`; with the `room` bound a pass never emits more than fits.)

Key facts:
* **Rows of a verify pass**: row 0 = frontier token `xF` at position `pos_ = F`; row j (1..kch) = draft `chain[j-1]`
  at position `F + j`; `am[j]` = the main model's choice for position `F + j + 1`. `M = kch+1` ≤ 16; default
  M = 8 (7 drafter tokens), M = 16 when the 16-row block is on or a long D0 copy is used.
* **Acceptance rule** is the same for greedy and sampling: longest prefix with `chain[j] == am[j]`; then `am[a]` is
  emitted too (a "free" token), so a pass emits `a+1` tokens (`a+2` with a leaf hit). Rows 0..a are committed.
* **Why sampling does not depend on speculation**: `am[j]` is a pure function of (logits of row j, seed, absolute
  position `F+j+1`, sampling params) — section 4. The logits of row j are those of plain decoding because rows are
  causally stacked and the arithmetic is batch-size independent. The drafter uses the *same* per-(seed, position,
  token) Gumbel noise (section 3.4) so its guesses match the main model's samples often; it never decides anything.
* **When nothing is drafted** (`room == 0`, i.e. the last token of the budget, or `NO_LOOKAHEAD`): `M = 1`,
  host `verify([xF], 1)` → the pass commits itself (`c1`), `commit(0)` only advances `pos_`.
* With `NO_LOOKAHEAD` (`bdraft=0, use_d0=0, use_d1=0`): `bdr = false`, `d0_lookup` returns 0, branch
  `specx.cpp:338-342` adds nothing → every pass is the 1-row self-committing pass. `passes == produced`.
* With `bdraft=0` but D0/D1 on (`specx.cpp:338-342`): `if n0 >= d0_priority_n: take_d0 elif use_d1 and
  next_tok(xQ,xP,xF) >= 0: extend() elif n0 > 0: take_d0`; host `verify`.

---

## 3. The draft model ("bdraft2")

### 3.1 Files and shapes

`bdraft2_k8.yskn` JSON model block: `arch bdraft2, layers 5, hidden 5120, ffn 17408, heads 32, kv_heads 8,
head_dim 128, vocab 248320, eps 1e-6, rope_theta 1e7, target_layers [5,19,33,47,61], block 8, mask_token 248070,
window 2048, top_k 16, rank 256, sub_vocab 0`.

46 coded tensors, all `role draft`, 8 bits/step (4 bits/weight), `l=12, d=2, t=256`, layout `warp`,
**no output rotation** (`rot_out: null`) (rows × cols):

| tensor | shape | index in `Bdr::L[i][k]` (`engine35.cpp:1624`) |
|---|---|---|
| `bdraft2.fc` | 5120 × 25600 | `fc` |
| `bdraft2.layers.i.q` | 4096 × 5120 | k=0 |
| `…k`, `…v` | 1024 × 5120 | 1, 2 |
| `…o` | 5120 × 4096 | 3 |
| `…gate`, `…up` | 17408 × 5120 | 4, 5 |
| `…down` | 5120 × 17408 | 6 |
| `…akp`, `…mkp` | 1280 × 5120 | 7, 8 |

35 bf16 vectors: `bdraft2.hidden_norm[5120]`, `bdraft2.norm[5120]`; per layer `ln1[5120]`, `ln2[5120]`,
`q_norm[128]`, `k_norm[128]`, `abase[4·5120]`, `mbase[4·5120]`; `bdraft2.sel.hp[256·5120]`,
`bdraft2.sel.pred[V·256]`, `bdraft2.sel.succ[V·256]`. There is no `bdraft2.head_sub` → `nsub = 0`, sub-head off.

**Linear layers.** All drafter products go through the same compressed integer GEMV as the main model
(`bdr_gemv`, `engine35.cpp:1755-1766`: `launch_lnord` with `xsum = 1`), with an int8 input `qx` + per-512-group
scale `alpha` and a 64-bit integer accumulator per output row. The float result is (`bdr_y`, `q35_bdraft.cu:474`)

```
y[m][r] = ( (float)acc[m][r] * us[m] ) * tau[r]        # two IEEE single multiplies, round-to-nearest
```

`us[m] = max_g alpha[m][g] * 2^-24` and `acc = Σ_g round(alpha_g * 2^24 / max alpha) * Σ_{j in g} w8[r][j]*qx[j]`
(`lnord.cu:53-54,256-258`); `tau[r]` is the tensor's per-row scale. The exact GEMV/`tau` definition belongs to the
weights spec (other area) — **UNCERTAIN here**: I did not re-derive `tau` and the code→int8 table for role `draft`.
Since the tensors have no output rotation, `y` is used directly (no inverse Hadamard).

**Input transform "a8"** (`a8_group512_tail`, `a8_prologue.cuh`): for each group of 512 consecutive inputs
`x[g*512 + t]`: `b[t] = x * sign[g*512+t]` → 512-point Walsh–Hadamard (unnormalised butterfly `a+b, a-b` for
h = 1,2,…,256) → `v = round_to_fp16(b * 0.044194173824159216)` (= 1/√512) → `alpha_g = max_t |v| / 127` →
`qx = clamp(rint(v / alpha_g), -127, 127)` (0 if alpha = 0). **One sign vector is used for every drafter input**:
`bdr_.signs = bdr_.fc.signs_in` (25600 entries, `engine35.cpp:1638`), indexed from 0 (so a 5120-wide input uses its
first 5120 entries, the 17408-wide one its first 17408, the 4096-wide one its first 4096). How `signs_in` is
generated (seed/rule in the container) is in the weights spec.

RMS norm used everywhere in the drafter (`bdr_row_r`): `r = rsqrt(mean_n(x²) + eps)`, `out = r * x * w[i]` with the
bf16 weight **as stored** (no `1 +`), `eps = 1e-6`.

### 3.2 Inputs

1. **Context rows — captured hidden states of the main model.** `load_bdraft` calls
   `set_capture({5,19,33,47,61})`; every main pass copies `h_` (the residual stream **after** layer l, f32, all M
   rows) into `cap_[k][row]` for those 5 layers (`engine35.cpp:832-833`; buffer `5 × Pmax_ × 5120` f32).
   **UNCERTAIN**: that `h_` at that point is exactly "hidden_states[l+1]" in every row-count mode (the layout
   options `lo_now_`/flow keep `h_` in the same place as far as I can tell; confirm with the forward-pass spec, or
   with `yskn_set_capture`/`yskn_capture_rows` on the CUDA build).
   The feature vector of a token is the concatenation `x[25600] = [cap0 | cap1 | cap2 | cap3 | cap4]` of its 5
   captured rows, **raw (no normalisation)** → a8 with 50 groups (`bdr_a8_cap_kernel`, `q35_bdraft.cu:490-500`:
   group g takes `cap[g/10][row][(g%10)*512 + t]`, sign index `g*512+t`).
2. **The frontier token's embedding** (block row 0) and the **mask token 248070** embedding (block rows 1..R-1):
   `bdr_block_ids` + `emb_src` + `bdr_embed` (`engine35.cpp:1796-1798`): `h[m][j] = bf16→f32(E[id_m][j])`, the main
   model's input embedding row as decoded bf16, unscaled.
3. Positions: context row of the token at absolute position p is stored at ring slot `p & 2047`; block row m is at
   position `pos_ + m`.

### 3.3 Context path (per committed token) — `bdr_ctx_rows`, `engine35.cpp:1767-1784`

For `m` rows at positions `p0 .. p0+m-1` (≤ 16 per launch):

```
qx, alpha = a8(x[25600])                              # launch_bdr_a8_cap
yfc[5120] = GEMV(fc, qx)                              # launch_bdr_post: acc*us*tau
c  = rmsnorm(yfc, hidden_norm);  qx, alpha = a8(c)    # launch_bdr_a8n (xn not kept)
for layer i in 0..4:                                  # NOTE: the same c for all 5 layers; no layer is run
    yk[8][128], yv[8][128] = GEMV(L[i].k, qx), GEMV(L[i].v, qx)
    for head h in 0..7:                               # launch_bdr_kv_ring
        k' = rope( rmsnorm128(yk[h], k_norm_i), pos = p0+row )
        ring_k[i][h][(p0+row) & 2047][0..128) = bf16(k')
        ring_v[i][h][(p0+row) & 2047][0..128) = bf16(yv[h])        # value: no norm, no rope
```

`rmsnorm128`: `rr = rsqrt(mean_128(x²) + eps)`, `v[d] = rr * x[d] * w[d]`.
`rope(v, pos)` over the **full 128 dims** (`bdr_norm_rope`, `q35_bdraft.cu:525-537`): `i = d & 63`,
`inv = 2^(-(2i/128) * 23.253496664211536)` (= 1e7^(-2i/128)), `ang = (float)pos * inv`,
`out[d] = d < 64 ? v[d]*cos(ang) - v[d+64]*sin(ang) : v[d]*cos(ang) + v[d-64]*sin(ang)`.
Ring storage: `rk[i]`, `rv[i]` = bf16 `[8 heads][2048 slots][128]`.

Entry points: `bdraft_ctx(row0, n, pos0)` (`:1785-1789`, used by prefill: rows `row0..row0+n-1` of `cap_` at positions
`pos0..`, in sub-chunks of 16) and the `n_ctx` part of `bdraft_step` (rows `0..n_ctx-1` of `cap_`, positions
`pos_ctx..`).

### 3.4 Draft block (one shot, non-autoregressive) — `bdr_block_impl`, `engine35.cpp:1791-1836`

`R = 8` (or 16) rows, `J = R-1` drafts. `p = pos_` (`bdr_.pos`). `D = 5120`, `FF = 17408`, `g(j) = j >> 4` (320
groups of 16 channels).

```
ids[0] = d_toks[0] (frontier token); ids[1..R) = 248070
h[m] = Emb[ids[m]]                                         # f32, R x 5120
for i in 0..4:
    # ---- attention sub-block ----
    x1[m] = rmsnorm(h[m], ln1_i); qx = a8(x1)                                    # launch_bdr_a8n
    dyn[m][0..1280) = GEMV(akp_i, qx)                                           # 4 x 320 gains
    u[m][j] = (abase_i[j]      + dyn[m][      g(j)]) * x1[m][j]
            + (m > 0 ? (abase_i[D + j] + dyn[m][320 + g(j)]) * x1[m-1][j] : 0)   # 2-tap causal "dynamic conv" over rows
    qx = a8(u)                                                                   # launch_bdr_dynconv_a8
    yq[m][32][128], yk[m][8][128], yv[m][8][128] = GEMV(q_i | k_i | v_i, qx)
    q[m][hq] = rope(rmsnorm128(yq[m][hq], q_norm_i), p+m);  k[m][hk] = rope(rmsnorm128(yk[m][hk], k_norm_i), p+m)
    att[m][hq][128] = attention (below)                                          # launch_bdr_attn_part + comb
    qx = a8(att[m] as 4096)                                                      # launch_bdr_attn_comb_a8
    y[m] = GEMV(o_i, qx)
    h[m][j] += (abase_i[2D + j] + dyn[m][640 + g(j)]) * y[m][j]
             + (m > 0 ? (abase_i[3D + j] + dyn[m][960 + g(j)]) * y[m-1][j] : 0)  # launch_bdr_post_conv_res
    # ---- GLU sub-block ----
    x1[m] = rmsnorm(h[m], ln2_i); qx = a8(x1)
    dyn[m] = GEMV(mkp_i, qx)                                                     # overwrites dyn
    u[m][j] = (mbase_i[j] + dyn[m][g(j)]) * x1[m][j] + (m>0 ? (mbase_i[D+j] + dyn[m][320+g(j)]) * x1[m-1][j] : 0)
    qx = a8(u)
    a = GEMV(gate_i, qx); b = GEMV(up_i, qx)                 # 17408 each
    s[j] = a[j] / (1 + exp(-a[j])) * b[j];  qx = a8(s)       # launch_bdr_glu_a8 (34 groups)
    y[m] = GEMV(down_i, qx)
    h[m][j] += (mbase_i[2D + j] + dyn[m][640 + g(j)]) * y[m][j] + (m>0 ? (mbase_i[3D+j] + dyn[m][960+g(j)]) * y[m-1][j] : 0)
hf[j] = rmsnorm(h[1+j], bdraft2.norm)   for j in 0..J-1    # row 1+j predicts the token at position p+1+j
```

`dyn` layout (`bdr_dynconv_a8_kernel`, `q35_bdraft.cu:504-523`; `bdr_post_conv_res_kernel`, `:588-605`):
`[0..320)` pre-gain current row, `[320..640)` pre-gain previous row, `[640..960)` post-gain current,
`[960..1280)` post-gain previous. `abase`/`mbase` are 4 × 5120 in the same order.

**Attention** (`bdr_attn_part_kernel`, `:114-216`; `bdr_attn_comb*`): 32 query heads, 8 KV heads (query head hq
uses KV head `hq / 4`), head dim 128, `scale = 1/sqrt(128) = 0.08838834764831845`. For block row m the key set is

```
ring entries of layer i at positions t with  max(0, p-2047, p+m-2047) <= t <= p-1      (committed context)
+ ALL R block rows j = 0..R-1 (their k[j], yv[j] from this layer)                      (no causal mask inside the block)
score(t) = scale * <q[m][hq], key_t>;   att[m][hq] = Σ softmax(score)_t * value_t
```

i.e. bidirectional within the block, causal to the context through the ring (window 2048). The CUDA code splits
the ring range into 32 chunks + 1 block chunk and merges partial (max, sum, weighted value) triples; any
numerically stable softmax over the union is equivalent (drafts need not be bit-exact, see 3.7).
The block's own K/V are **not** written to the ring.

**Output head (SHEAD, `launch_bdr_shead`, `q35_bdraft.cu:738-975`)** — used because `sk_m = 512 > 0`:

```
for each draft row j:
    qv[k]  = Σ_i hf[j][i] * bt[k][i]                 k in 0..511                (bdr_sk_proj_kernel)
    s[v]   = ( Σ_k qv[k] * (float)a[v][k] ) * as[v]   for all v in 0..V-1        (bdr_sk_score_kernel)  low-rank score
    C      = the M = 512 tokens with the largest s[v]                            (hist2 + collect)
    cv[v]  = ( Σ_i hf[j][i] * (float)w8[v][i] ) * ws[v]   for v in C            (bdr_sk_rescore_kernel) int8 copy of lm_head
    val[j][0..16), idx[j][0..16) = top 16 of C by (cv desc, token id asc)         (bdr_sk_top16_kernel)
```

The top-512 selection in CUDA is a two-level histogram on the monotone float key
(`key(f) = sign ? ~bits : bits | 0x80000000`), threshold at 21-bit key precision: everything with a larger 21-bit
key is taken, and tokens *equal* to the threshold key fill the remaining slots in a race-dependent order
(`:882-897`). So the boundary of `C` is not deterministic in CUDA; an exact top-512 by `s` is a valid implementation.
"score grid 72 blocks" in the log is only the kernel launch grid (`bdr_shead_grid`, occupancy × SM count); it has no
meaning for the port.

**`.shead` file layout** (`load_shead`, `engine35.cpp:1700-1743`; verified on the file: 1 411 010 584 bytes):

| offset | content |
|---|---|
| 0 | magic `"SHEAD001"` (8 bytes) |
| 8 | int32[4] = {V = 248320, H = 5120, r = 512, 0} |
| 24 | `bt` float32[r × H] (row k = projection vector k) — 10 485 760 B |
| … | `a` int8[V × r] — 127 139 840 B |
| … | `as` float32[V] |
| … | `w` int8[V × H] — 1 271 398 400 B |
| … | `ws` float32[V] |
| end | must be EOF ("trailing bytes" is an error) |

Fallback when no SHEAD (`sk_m == 0`, not the shipped configuration): GEMV of the **main model's** `lm_head` on
`a8(hf)` → full logits → `launch_bdr_topk16` (exact top-16, ties → smaller id) (`engine35.cpp:1822-1832`).

**Candidate re-scoring and chain selection** (`launch_bdr_hproj` + `bdr_select3_kernel`,
`q35_bdraft.cu:299-306,388-459`): a rank-256 bilinear "previous token → next token" correction.

```
hp[j][r] = Σ_i hf[j][i] * Hp[r][i]            r in 0..255   (bdraft2.sel.hp, bf16 [256][5120])
prev = d_toks[0]                              (frontier token)
for j in 0..J-1:
    for c in 0..15:  score[c] = val[j][c] + Σ_r pred[prev][r] * hp[j][r] * succ[idx[j][c]][r]
    greedy (cpl <= 0):   b = argmax_c score[c]                 (ties → lowest c)
    sampling (cpl > 0):  see below
    t = idx[j][b];  d_toks[1+j] = t;  out[j] = t;  prev = t
    alt[j]  = 2nd-best candidate's token (or -1), marg[j] = score(best) - score(2nd) (1e30 if none)
    alt3[j] = 3rd-best candidate's token (or -1)
```

`cpl` is set per draft by `bdraft_step`: `cpl = (sampling on && version == 3) ? cpl_pm : -1`, `cpl_pm = 850`
(`engine35.cpp:1850`, `engine35.hpp:183`). Sampling branch (drafter temperature `td = cpl/1000 = 0.85`, fixed,
independent of the request temperature):

```
order the 16 candidates by (score desc, token id asc) → rank rk[c]
z[c] = (score[c] - score[top]) / td;  e[c] = exp(z[c])            (double)
pre[c] = Σ e over candidates ranked before c;  tot = Σ e
val[c] = (pre[c] < 0.95 * tot) ? z[c] + gumbel(seed, pos_ + 1 + j, idx[j][c]) : -inf     # same hash as the main sampler
b = argmax val (ties → better rank);  alt/alt3 = 2nd/3rd by val;  marg = val[b] - val[2nd]
```

### 3.5 Side candidates ("HCAP", sampling only) and the row table ("wk" / "lfrow")

* **Row table** `wk[48]` (uint8, device buffer `d_wk_`; `lfrow_kernels.cuh`): lets the rows of one verify pass form a
  small tree instead of a chain.
  `wk[0..15]`: depth of row t (its position is `pos_ + depth`; default `t`).
  `wk[16..31]`: order in which the DeltaNet recurrence visits rows; an entry with bit `0x80` is a **leaf**: it is
  computed from the state reached so far but its state update is discarded (`wk_state_kernel`, `:110-141`).
  `wk[32..47]`: candidate rank of leaf rows (1 = 2nd, 2 = 3rd).
  In attention a leaf row at depth d attends cache positions `< pos_ + d` plus itself, while its own K/V are
  stored at slot `pos_ + row` (`sl_of`, `:246-247`); the sampler uses `pos_ + depth + 1` (`q35_sample.cu:180-181`).
  "lfrow" = leaf row. `wk_plain_table()` = identity (`wk[i] = wk[16+i] = i`), under which the wk kernels equal the
  plain ones; the default loop turns the tables on (`set_opt(6,1)`) whenever HCAP is available and uploads the
  plain table before each verify pass that the drafter did not prepare (`engine35.cpp:1524`).
* **HCAP** (`bdr_hcap_kernel`, `q35_bdraft.cu:659-706`; tables `src/hcap_tables.inc`: 11 margin edges, then
  `PI`, `RHO`, `RHO3` as [7 depths][12 bins]): runs after `bdr_select` only when `hb_go` (= `hb_pass`) is set. From
  the margins it estimates `p_j` = P(draft j accepted), `R[j]` = P(chain reaches j), and the value of a leaf
  `v2[j] = R[j]*(1-p_j)*RHO[j][bin]` (2nd candidate) / `v3[j] = …*RHO3` (3rd). For `L = 7,6,5,4` it greedily picks
  the `7-L` best leaves among depths ≤ L and keeps the L with the largest expected tokens
  `1 + Σ_{q=1..L} R[q] + Σ leaf values`. It then writes the row table, overwrites `d_toks[L+1 ..]` with the leaf
  tokens (`alt` / `alt3` of depth d-1) and `hb_cfg = {L, nleaf, depth[0], depth[1], depth[2]}`. Always 8 rows.
  **The search loop runs only when `cpl > 0`** (sampling); with greedy it writes the identity table for 8 rows
  (`L = 7, nleaf = 0`).
* Leaf hit (section 2.4): if the chain breaks at index a and a leaf of depth a+1 holds exactly `am[a]`, the leaf's
  own prediction `am[leaf]` is the next token as well. `lfrow_promote(src = leaf row, dst = a+1)`
  (`engine35.cpp:877-900`, `wk_promote2_kernel`) copies everything row-indexed from row `src` to row `dst`: per linear
  layer `pmix, pab, tq, tk, tv, tbeta, tg`; per attention layer the K/V rows (and scales) at cache slot
  `pos_+src → pos_+dst`; `xn_`, `d_next`, `d_toks`, and the 5 captured rows. Then `commit(a+1)`.

**For the port HCAP is optional**: report `hcap_on() == false` (CUDA: `YSKN_HCAP=0`) and no table is ever
non-identity; the emitted ids are unchanged by design (leaves only add accepted tokens).

### 3.6 `bdraft_step(n_ctx, pos_ctx, draft)` and catch-up after a commit

`engine35.cpp:1838-1860`. `n_ctx` in 0..16, `draft` in {0, 1 (8-row block), 2 (16-row block)}:

```
if n_ctx: cpos = pos_ctx
if draft: bdr.pos = pos_; hb_go = hb_pass; cpl = (smp_ && version == 3) ? cpl_pm : -1
if n_ctx: bdr_ctx_rows(cap rows 0..n_ctx-1)        # section 3.3: rings get positions pos_ctx .. pos_ctx+n_ctx-1
if draft: bdr_block(draft >= 2 ? 16 : 8)           # section 3.4 (+3.5); writes d_toks[1..R)
```

Catch-up contract: after `commit(n_commit)` of a pass that started at frontier F, the next `bdraft_step` gets
`n_ctx = n_commit+1, pos_ctx = F`: the captured hiddens of the rows just committed (rows `0..n_commit` of the last
verify pass — row 0 is the old frontier token, now run through the main model) become ring entries
`F .. F+n_commit`. Hence when drafting at `pos_ = p` the rings hold every position `< p` (up to 2048 back), and the
frontier token itself enters only as an embedding (block row 0). The `cap_` buffer must therefore survive from the
verify pass until the next `bdraft_step`, and `lfrow_promote` moves the captured row too. After the loop a final
`bdraft_step(dctx_n, dctx_pos, 0)` flushes. During prefill `bdraft_ctx(0, M, pos_)` does the same per chunk.
The drafter consumes **captured rows of the pass that committed them**, never recomputed ones.

### 3.7 Exactness requirements

The drafter only proposes. Its arithmetic (float softmax, dynamic conv, SHEAD selection, even the boundary of the
512-candidate set) **need not be bit-identical to CUDA**; only acceptance rate (speed) is at stake. What must hold:
(a) ring contents at snapshot/restore are consistent (save/restore all 10 rings with the snapshot), (b) with
sampling it uses the same `gumbel(seed, pos, token)`.

Every `launch_bdr_*` in `q35.hpp:178-217` → section: `rownorm`, `a8`, `post`, `gconv`, `qk_rope`, `ring_store`,
`attn`, `silu_mul`, `gather_feats` are the unfused older variants (same math as the fused `a8n`, `a8_cap`,
`dynconv_a8`, `qkv_post`, `kv_ring`, `attn_part`+`attn_comb_a8`, `post_conv_res`, `glu_a8` used above; `a8` with
`w == NULL` is the raw-feature path of `bdraft_debug`); `embed`, `block_ids` (3.4 start); `topk16`, `map_ids`
(fallback head); `shead`, `hproj`, `select` (3.4); `hcap` (3.5).

---

## 4. Sampling and arg-max in the main model

### 4.1 Greedy arg-max (`argmax_stage1/2`, `src/misc_kernels.cu:57-96`)

Over the V float32 logits of a row: larger value wins; **equal values → the smaller token id**; start value
`-FLT_MAX` with id `0x7fffffff` (a NaN logit never wins). The same order `(value desc, id asc)` defines the top-8
(`topk8_*`, `:100-147`) and the sampler's candidate list (`kj_better`, `q35_sample.cu:29`).

### 4.2 `launch_q35_smp` (`src/q35_sample.cu`; default kernel `smp2_kernel`, `:131-207`)

Runs after the arg-max in `head_slice` when sampling is on (`engine35.cpp:814-820`) and **overwrites `d_next[r]`**
with the sampled id. Parameters (`Q35SmpParams`, `q35.hpp:114-116`): `temp, top_p, min_p` doubles, `top_k` (1..20),
`version` (3), `seed` (uint64 holding 32 bits), `pm` (0; "precise math" variant only with `YSKN_Q35_PMATH`).

```
k = top_k
cand = the k largest logits of the row, sorted by (value desc, id asc): values sv[0..k) (float32), ids si[0..k)
        (the kernel also stores the first 8 as that row's top-8)
depth = row index within the pass (or wk[row] with a row table)
pos   = (int64) d_pos + depth + 1            # absolute position of the token being chosen
for j < k:  z[j] = ((double)sv[j] - (double)sv[0]) / temp;   w[j] = exp(z[j])          # double precision
tot = Σ_{j<k} w[j]
n = k; run = 0; for j in 0..k-1: run += w[j]; if run >= top_p * tot: n = j+1; break       # nucleus over the top-k
if min_p > 0: m = 1; while m < n and w[m] >= min_p: m++; n = m                            # w[m] = p_m / p_top
pick = n-1
version 3 (Gumbel-max):
    best = -inf; for j in 0..n-1: g = z[j] + gumbel(seed, pos, si[j]); if g > best: best = g; pick = j
version 2 (inverse CDF, not used by Python):
    u = uniform(seed, pos) * Σ_{j<n} w[j]; run = 0; for j < n: run += w[j]; if u < run: pick = j; break
d_next[row] = si[pick]
```

Hash (`src/smp_hash.cuh`), all uint64 wrap-around:

```
mix(z):  z += 0x9E3779B97F4A7C15; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
         z = (z ^ (z >> 27)) * 0x94D049BB133111EB; return z ^ (z >> 31)
gumbel(seed, pos, v):
    h = mix( mix( mix(seed) ^ (uint64)pos ) ^ ( (uint64)(uint32)v * 0xD6E8FEB86659FD93 ) )
    u = ((double)(h >> 11) + 0.5) * 2^-53                     # in (0,1)
    return -log(-log(u))                                      # double
uniform(seed, pos) = (double)( mix( mix(seed) ^ (uint64)pos ) >> 11 ) * 2^-53       # version 2 only
```

Properties: the choice at a position depends only on that row's logits, `seed`, the absolute position and the
parameters → independent of how rows were batched or drafted. The token returned by prefill for position n is
sampled the same way (`pos = chunk start + row + 1`).
**UNCERTAIN / risk**: `exp`/`log` are CUDA device double routines; a host or SYCL libm may differ in the last ulp,
which can flip a pick only in a near-tie. No shipped check compares sampled output across machines (selfcheck and
bench are greedy), so this is not a blocker.

The logits themselves (`argmax_stage1<true>`, `misc_kernels.cu:67-73`): `x[v] = ((float)acc[v] * us[row]) * rr * tau[v]`
with `rr = rsqrt(Σ_b S[row][b] / 5120 + eps)` (final-norm factor) — details in the forward-pass spec.

---

## 5. Staged implementation

### Stage (i): `yasakani selfcheck` PASS, no speculation

There **is** a cfg that disables look-ahead (`bdraft=0, use_d0=0, use_d1=0`), but selfcheck does not use it: it
sends the default cfg. Because the emitted ids are by construction independent of drafting, the port may
**ignore the drafting fields** and run one row per pass regardless. Minimum:

1. Export all 14 symbols (1.1–1.3). `yskn_prefill/decode/logits/nll/state_hash` may return -1 with a message.
2. `yskn_open`: load the model, honour `max_ctx` (round up to 64). Device index → SYCL device choice.
3. `yskn_load_bdraft`: return 0 (optionally validate the file); remember "loaded".
4. `yskn_set_opt`: accept keys 10, 11, 12, 13, 16, 29 with the validation of 1.3 and store them. **The warm-up runs
   with temperature 1.0**, so the sampler path is entered before selfcheck: either implement section 4 now (small,
   host-side over the logits is fine) or fall back to arg-max — warm-up output is discarded, nothing checks it.
5. `yskn_reset`: zero DeltaNet states + conv history, `pos = 0`, drop snapshot/live info.
6. `yskn_spec_generate`, simplest correct form:
   ```
   cancel = 0; validate n_cfg >= 12, gamma in 0..15
   reset(); start = 0; mode = 0                       # (prefix reuse can wait; then always report 0/0)
   feed prompt[0..n) (committing), first = token for position n; out_ids[0] = first
   produced = 0; stop = is_eos(first); passes = 0
   while produced < n_gen and not stop and not cancel:
       tok = S.back(); run one committing row at pos; pos++; t = arg-max or sample (pos used = new pos)
       out_ids[1 + produced++] = t; S.push(t); passes++; stop = is_eos(t)
   stats[0] = passes; stats[1] = produced; stats[30] = produced; stats[31] = stop; stats[41] = mode; stats[42] = start
   times_ms[0], [1]
   ```
   Careful: with `snap = 1` the CUDA engine does **not** require the caller to reset — a fresh prompt triggers
   `engine.reset()` inside prefill. The port must do the same (or implement mode 1/2); it must never assume
   `pos == 0` on entry.
7. Outputs that must be **identical to CUDA** for PASS: the three id sequences (ids `out_ids[0..produced]`,
   including the trailing stop id of case 1) → the main model's arg-max must agree with CUDA at every one of the
   3 + 160 + 32 steps, with the tie rule of 4.1. Nothing else of the engine's numerics is observed by selfcheck.
   Outputs the Python layer needs to be *well-formed* (not equal to CUDA): `out_ids` written progressively and
   never negative; `stats[30]`, `stats[31]`, `stats[0]`, `stats[41]`, `stats[42]`; `times_ms`.

### Stage (ii): bench

Nothing new functionally: `reset` + greedy `spec_generate` with `eos1 = eos2 = 0x7FFFFFFF` (never stops early) and
`n_gen = 511`. Bench prints `tokens/s = (produced+1)/gen_ms` and `tokens/pass = (produced+1)/max(1, stats[0])`, so
`stats[0]` must be the real number of main-model passes and `times_ms[1]` must cover only the loop.
Worth adding here because they are pure wins without the drafter:
* multi-row prefill (chunks; identical ids required), and the last-row-only head during prefill;
* prefix reuse: mode 1 (live) is only bookkeeping (`sh_cur`, an epoch counter); mode 2 needs the host snapshot of
  `S`/history (2.2). Report `stats[41]`, `stats[42]` truthfully — `cached_tokens` is shown to API clients;
* D0/D1 host drafts + multi-row verify + `replay_commit` (sections 2.1, 2.3) — this already exercises the whole
  verify/commit machinery with host-supplied rows (`verify`), before any drafter code exists.

### Stage (iii): full speculation

In order: (1) non-committing M-row verify (M ≤ 16) with kept per-row DeltaNet intermediates, provisional KV rows,
`replay_commit`; test: ids equal to stage (i) for every M and every acceptance pattern (this is the property
`lookahead_check.py` verifies). (2) capture of layers 5/19/33/47/61 for all rows of every pass, drafter context
path + rings, included in the snapshot. (3) draft block with the SHEAD head and `sel` re-scoring, greedy.
(4) the control loop of 2.4 including the 8/16-row controller and D0-vs-drafter averaging. (5) sampling: main
sampler (section 4) + drafter Gumbel branch (`cpl = 850`). (6) optional: HCAP + row tables + `lfrow_promote`.
Validation oracle: on the CUDA build, `yskn_bdraft_debug`, `yskn_bdraft_ring`, `yskn_capture_rows`,
`yskn_bdraft_draft` (`c_api.cpp:117-125`) expose drafter hidden states `hf` (7×5120), top-16 ids/values, the chosen
path and ring rows.

Outputs that must be identical **between the port's own configurations** (not vs CUDA) once speculation is on:
the id sequence for a given (prompt, sampling params, seed) regardless of cfg, row count, acceptance pattern and
prefix-reuse mode. That requires bit-identical logits for a row whether it is computed alone, in an M-row verify
pass, or in a prefill chunk — true in CUDA because all matrix products are exact integer sums and the per-row float
steps do not depend on M (docs/TECHNOLOGY.md §4, §7). If the port cannot guarantee this, the Python layer still
works, but look-ahead on/off and cached/uncached requests may produce different text.

---

## 6. Open points (UNCERTAIN)

1. `tau`, `us`, the integer accumulator and the code→int8 table for the `draft` role (8 bits/step, no output
   rotation), and the generation of `signs_in` for `bdraft2.fc` — defined by the GEMV/format spec; not re-derived here.
2. That the captured `h_` after `layer(l)` equals the residual stream after layer l in all row-count modes (3.2).
3. Whether a selfcheck PASS needs more than arg-max agreement in practice: the 160-token "thinking" case has no
   margin information; a port whose float steps differ from CUDA in rounding could flip a near-tie. The CUDA build
   on the RTX 5060 Ti passes with the same reference (PORT_NOTES.md), so GPU model does not matter for CUDA.
4. Device libm (`exp`, `log` in double) differences in the sampler (4.2) and `sincosf`/`exp2f`/`__expf` in the
   drafter (harmless there).
5. The CUDA SHEAD top-512 boundary is race-dependent (3.4), so even CUDA drafts are not strictly reproducible;
   pass counts (`stats[0]`) may vary slightly between runs, ids do not.
6. The 8/16-row controller variables are function-`static` in CUDA (`specx.cpp:146-155`) but are re-initialised at
   the start of every `generate` (`:179`), so per-call state is equivalent; the cumulative counters
   `b16_passes/b16_all` are only for a log line.
7. `yskn_state_hash` with KV5 packing hashes packed K/V rows; exact bytes depend on the KV cache format (other area).
