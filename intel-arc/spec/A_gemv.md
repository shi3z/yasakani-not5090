# A. Model loading and the compressed matrix product (gemv), default configuration

Source root: `/home/shi3z/yasakani-src/yasakani` (commit c0e11b9). All `file:line` references are relative to it.
The source has had its comments stripped; every statement below is derived from code, not from comments.
Checked against the shipped file `/data/yasakani/model-speed/qwen38_27b_speed.yskn` (header read directly).

Notation: `RN(x)` = IEEE float32 round-to-nearest-even of the exact result. All float ops are float32, one rounding per
operation, no FMA (`--fmad=false`, CMakeLists.txt:50; most kernels also use explicit `__fmul_rn/__fadd_rn/__fdiv_rn`).
`K512 = 0.044194173824159216f` (= 1/sqrt(512) as a float32 literal).
`rf16(x)` = float32 -> binary16 (RNE) -> float32. `rbf(x)` = float32 -> bfloat16 (RNE) -> float32 (`bf2f(u) = bits(u << 16)`).
`rint_rn(x)` = `__float2int_rn` = round to nearest, ties to even, to int32.

---------------------------------------------------------------------------------------------------------------------

## 0. Summary of the default path (what to implement)

For one matrix `W` (N rows = `physical_rows`, Kc columns, G = Kc/512 column groups) and M input rows:

```
inputs : qx[M][Kc] int8, alpha[M][G] float32            (section 3)
weights: w8[N][Kc] int8 (from codes + lut8), tau[N] f32 (section 2)

for tok in 0..M-1:
    m      = max_g alpha[tok][g]                        # fmaxf fold starting from 0.f
    rm     = m > 0 ? RN(16777216.f / m) : 0.f
    us[tok]= ldexpf(m, -24)                             # exact
    for n in 0..N-1:
        acc = 0 (int64)
        for g in 0..G-1:
            A   = (int64) rint_rn( RN(alpha[tok][g] * rm) )                 # 0 .. ~2^24
            dot = sum_{c=512g}^{512g+511} (int32) w8[n][c] * (int32) qx[tok][c]   # |dot| <= 512*127*127 < 2^23
            acc += A * (int64) dot
        part[tok][n] = acc                              # exact integer, order independent
y_rot[tok][n] = RN( RN( (float)acc * us[tok] ) [ * r_tok if the input was "normed" ] ) * tau[n] )   # section 4.3
```

then output rotation (if the tensor has one): FWHT512 per 512 rows, `* K512`, `* signs_out`, round to bf16 (section 4.4).

Everything else in the CUDA (workers, tiles, `frag`, `lno`, LO layout, PFA/G6, PDL, L2 prefetch) is layout or scheduling and
does not change a single bit of the result, because the accumulation is an exact int64 sum.

---------------------------------------------------------------------------------------------------------------------

## 1. Which modes are ON by default

How the default is established:

* Python opens with `flags = 0`: `self._f["yskn_open"](..., self.max_ctx, int(device), 0)` (python/yasakani/engine.py:104).
  `flags_` is stored (src/engine35.cpp:184) and never read again in Engine35.
* Before opening, the process environment is forced to exactly `DEFAULT_ENV` + two paths; any other `YSKN_*` variable is
  removed (python/yasakani/runtime.py:27-43, 83-101; engine.py:96). So an env var not in `DEFAULT_ENV` is definitely unset.
* After opening: `yskn_load_bdraft` (engine.py:112). `yskn_set_opt` is called by Python only for sampling: keys
  11 (top_p), 12 (top_k), 13 (seed), 10 (temperature), 16 (sampler version), 29 (min_p) (engine.py:20, 166-177).
* `yskn_set_opt` -> `Engine35::set_opt` (src/c_api.cpp:92, src/engine35.cpp:908-962).
* The library is compiled with `YSKN_KVX_MODE=3` (CMakeLists.txt:46).

`DEFAULT_ENV` (runtime.py:27-43): `YSKN_BDR_SHEAD_M=512, YSKN_Q35_G6=1, YSKN_Q35_KV8=1, YSKN_Q35_KV_FP16=0,
YSKN_Q35_LAYER_OVERLAP=1, YSKN_Q35_LAYER_ROWS=4096, YSKN_Q35_PFALT=1, YSKN_Q35_PMAX=1024, YSKN_Q35_SHARED_WEIGHTS=1,
YSKN_Q35_ATX=1, YSKN_TB_CAP=1, YSKN_TB_VIT2=1, YSKN_ATX_PK3_PLUT=1, YSKN_ATX_PKV=3, YSKN_ATX_VSP=7` + `YSKN_BDR_SHEAD=<path>`,
`YSKN_TB_PROFILE=<path>`.

### 1.1 Modes that affect the gemv / its inputs and outputs

| variable | default value | set by | state in shipped config | what it changes |
|---|---|---|---|---|
| `xsm_` | 1 (engine35.hpp:248) | env `YSKN_Q35_XSUM` (engine35.cpp:211), not in DEFAULT_ENV | **ON** | exact int64 "XSUM" accumulation; `part` is int64, consumers get `us_`/`S_` (sections 4, 5) |
| `xsmc_` | false; `= xsm_ != 0` at the start of every `pass_body`, false at its end (engine35.cpp:825, 841) | derived | **ON inside every pass** | the per-pass copy of `xsm_` that all `if (xsmc_)` branches test |
| `xsm_part_` | 0 (hpp:254) | env `YSKN_Q35_XSM_PART` (cpp:212) | OFF | per-worker int64 partials instead of one atomic sum; `nmode()` would return 2 |
| `opt_lo_` | 1 (hpp:307) | env `YSKN_Q35_LO` (cpp:192), set_opt 28 (cpp:914) | **ON** | `lo_now_` = interleaved qx layout for M <= 8 (layout only, section 3.4) |
| `lo_now_` | `opt_lo_ && xsmc_ && M <= 8 && !opt_flow_ && !(opt_fin_ && M == 1) && !(chk_on_ && !chk_.empty())` (cpp:827) | derived | **ON for M <= 8**, OFF for M >= 9 | qx buffer layout only |
| `opt_flow_` | 0 (hpp:292) | env `YSKN_Q35_FLOW` (cpp:349), set_opt 14 | OFF | fused MLP kernel chain (`flow_mlp`); not used |
| `opt_fin_` | 0 (hpp:297) | env `YSKN_Q35_FIN` (cpp:348), set_opt 7 | OFF | fuses residual+norm+A8 into the gemv (`lno_fin_res_norm`), uses `qx2_/alpha2_`, `in2_`; not used |
| `in2_` | false (hpp:297) | set only by the FIN path (cpp:670) | OFF | gemv always reads `qx_`/`alpha_` (cpp:409) |
| `pfa_` | 1 (hpp:246) | env `YSKN_PFA` (cpp:193) | **ON** | with XSUM: gemv for M > 16 goes to the PFA GEMM (`launch_pfa`), cpp:416-419 |
| G6 (`g6_.weights`) | allocated iff `YSKN_Q35_G6 != 0 && xsm_ && !xsm_part_` (cpp:314) | env `YSKN_Q35_G6=1` (DEFAULT_ENV) | **ON** | gemv for M >= 256 pre-decodes the weights to int8 and runs a GEMM (`launch_q35_g6`, q35_g6.cu:310-311); same math |
| `Mmax_` | 64 (hpp:238) | constant | 64 | attention / scratch row limit of a normal pass |
| `HM_` | 16 (hpp:313) | constant | 16 | max rows of one head launch; also the limit of the LNORD XSUM gemv (lnord.cu:379 `P.xsum && P.M > 16` -> not eligible) |
| `Pmax_` | `pfa_ ? 512 : Mmax_` (cpp:194), then `YSKN_Q35_PMAX` if 512..2048 and multiple of 64 (cpp:195-196) | env | **1024** | rows of the scratch buffers (`h_`, `qx_`, `alpha_`, `us_`, `S_`, ...) |
| `PM_` | `= Pmax_` (cpp:197); env `YSKN_Q35_PM` (cpp:213) | env (unset) | **1024** | rows per prefill chunk (cpp:1203) |
| `tq_qx_` | nullptr (hpp:330) | set only inside `prefill_vsp` (cpp:1128, 1153) | null in decode/verify | alternate qx buffer during layer-wise prefill |
| `layer_rows_`, `ovl_` | 0, 0 | env `YSKN_Q35_LAYER_ROWS=4096`, `YSKN_Q35_LAYER_OVERLAP=1` (cpp:198-199) | **4096, 1** | prefill of n >= 1024 tokens runs layer by layer (`prefill_vsp`, cpp:984); math of each gemv unchanged |
| `chk_on_` / `chk_` | 1 / empty (hpp:206) | `yskn_chk_set` (not bound by Python, _native.py:25-40) | effectively OFF | weight-checksum check after each gemv (`chk_after`, cpp:429-439) |
| `kskip_` | 0 | only `time_verify` (cpp:1343) | OFF | debug skip mask (`ks()`, `G`/`S` booleans) |
| `l2pf_`, `l2pf2_mb_` | 0, 0.0 (hpp:301, 304) | env `YSKN_L2PF`, `YSKN_L2PF2`; set_opt 25, 27 | OFF | L2 prefetch of the next code stream; `pf_on()` false, `pf2_live_` false |
| `kz_` | 0 (hpp:289) | env `YSKN_Q35_PMATH`, set_opt 21 | OFF | "portable math": `q35_rsqrt` = `1/sqrt` instead of `rsqrtf`, etc. **OFF means the CUDA `rsqrtf/expf` library functions are used** (see uncertainties) |
| `emb_coded_` | false | true iff the file has tensor `embed_tokens` (cpp:272-298) | **ON** | embedding is decoded from codes on the fly (section 7.2) |
| `emb_host_` | 0 | env `YSKN_Q35_EMB_HOST` | OFF | irrelevant when the embedding is coded |
| `YSKN_Q35_EMB1` | unset | env (q35_kernels.cu:1571) | OFF | selects `q35_embed_blk_kernel` (the default) over `q35_embed_kernel`; same math |
| `want_` | `sm_count * 8` (cpp:221) | env `YSKN_Q35_WANT` | sm_count*8 | only enters `nw_big` (parallel split) |
| `YSKN_Q35_NW` | table `{down 9, out 4, o 4, gate 5, up 5, qkv 8, z 8, q 4, k 4, v 4}` (cpp:123) | env (unset) | table | `n_workers` per tensor (parallel split only) |
| `YSKN_Q35_HEAD_W` | `cols/512 = 10` (cpp:268) | env (unset) | 10 | head `n_workers` |
| `YSKN_LNO_PREW/LOOK/PRE/XSMR/CFG` | 1 / 0 / 3 / 1 / -1 (lnord.cu:387-390, 415) | env (unset) | defaults | kernel shape / prefetch variants of LNORD; no numeric effect |
| `YSKN_Q35_SHARED_WEIGHTS` | "1" | DEFAULT_ENV | ON | weights shared between engines of one process (cache keyed by file); no numeric effect |
| `YSKN_Q35_PFALT` | 1 | DEFAULT_ENV (q35_kernels.cu:245) | ON | alternative post kernels when M > 64 (`q35_pf_launch`, q35_kernels.cu:602); prefill only |

`nmode(M, normed)` (hpp:255): `M > 16 ? (normed ? 3 : 1) : (xsm_part_ ? 2 : 0)` -> **0 for M <= 16**, 1 or 3 for M > 16.
`pt(t, M)` (hpp:253): `(xsmc_ && M > 16) ? ybuf_ + (t.part - part_) : t.part` -> int64 `part_` for M <= 16, float `ybuf_` for M > 16.
`nwk(t, M)` (hpp:247): `(pfa_ && M > Mmax_) ? 1 : t.dev.n_workers`; only used by non-XSUM consumers (ignored with XSUM).

### 1.2 Modes outside the gemv (listed so the default path is unambiguous; details belong to other areas)

| variable | default | set by | state |
|---|---|---|---|
| `kv8_` | 1 (hpp:267) | env `YSKN_Q35_KV8=1` | ON: int8 K/V rows + float scale |
| `kv_fp16_` | 0 (hpp:271) | env `YSKN_Q35_KV_FP16=0` | OFF |
| `tbcap_` | 0 (hpp:268) | env `YSKN_TB_CAP=1` (cpp:207, KVX_MODE 3) | **ON**: packed KV ("KV5") streams from `YSKN_TB_PROFILE`; `tbcap_kb_ = g_tb_kb` |
| `atx_` | 1 (hpp:281) | env `YSKN_Q35_ATX=1`, set_opt 9 | ON; `atx_splits_ = 42`, `atx_short_ = 31`, `atx_long_ = 4096` |
| `atx_vsp_` | 3 (hpp:285) | env `YSKN_ATX_VSP=7` (cpp:357), set_opt 22 | **7** |
| `fa_` | 1, `fa_splits_ = 31` (hpp:280) | env `YSKN_Q35_FA`, set_opt 8 | set, but unused because `atx_` wins (cpp:707, 716-718) |
| `opt_kconv_` | 1 (hpp:366) | set_opt 5 | ON: conv outputs kept in `Y.tq..tg` for M <= HM_ (cpp:785) |
| `opt_lfrow_` / `wk_now_` | 0 (hpp:362) | set_opt 6; `specx` sets it to 1 during `yskn_spec_generate` when `bdr && hcap_on()` (specx.cpp:195); `bdr_.hb_on` defaults to 1 (cpp:1693) | **ON during generation** for passes with `!commit && M <= HM_ && head_mode == HEAD_ALL` (hpp:364). Does not touch the gemv |
| `mtp_.on` | false | `load_draft` (not called by Python) | OFF |
| `bdr_.on` | false | `yskn_load_bdraft` (engine.py:112) | **ON**; SHEAD loaded with M = 512 (`YSKN_BDR_SHEAD`, `YSKN_BDR_SHEAD_M`, cpp:1664-1671) |
| `cap_` (capture) | null | `load_bdraft` calls `set_capture({5,19,33,47,61})` (cpp:1650) | **ON**: `h_` copied after those layers (cpp:832-833) |
| `smp_` | 0 (hpp:312) | set_opt 10 with value > 0 | OFF for greedy (selfcheck uses temperature 0, cli.py:421); ON for chat/serve defaults (temperature 1.0); `smp_p_.version = 3` (cpp:345, set_opt 16) |
| `opt_replay_graph_`, `opt_single_commit_`, `opt_catchup_nosync_` | 1, 1, 1 (hpp:361) | set_opt 1, 2, 3 | ON |
| `opt_mtp_log_` | 0 | set_opt 4 | OFF |
| `opt_rp_async_`, `opt_rp_defer_` | 1, 0 (hpp:320, 323) | env `YSKN_Q35_RP_ASYNC`, `YSKN_RP_DEFER`; set_opt 15, 26 | ON, OFF |
| `uat2_`, `visual_on_` | 0, false | - | OFF |

---------------------------------------------------------------------------------------------------------------------

## 2. Load-time processing

`Engine35` does **not** call `Model::load` (src/loader.cpp:69, that is the path of the older engine). It opens the container
itself (`m_.c.open(path)`, engine35.cpp:214) and calls `Engine35::up()` per matrix and `Engine35::vec()` per vector.

### 2.1 Container (src/format.hpp:26-65, docs/FORMAT.md)

* bytes 0..3 `YSKN`, u32 version = 1 at 4, JSON length at 8 (the reader takes only the low 32 bits, format.hpp:29), JSON at 0x20.
* All data are reached through absolute file offsets (`at<T>(off) = base + off`, format.hpp:64).
* Checked constraints per tensor: `d == 2, t == 256, 8 <= l <= 14, 4 <= bits <= 8, cols % 256 == 0` (format.hpp:56).
  `upload_linear` additionally requires `l == 12` (loader.cpp:30).
* `rot_out` present and kind != "none" -> `has_rot_out` (format.hpp:50). In the shipped file: every `body` tensor has
  `rot_in = {H512, seed 7, offset 0, mulberry32_7919_13}` and `rot_out = {H512, seed 7, offset = cols, mulberry32_7919_13}`;
  `lm_head` and `embed_tokens` have `rot_out = null`.

Shipped target model (`qwen38_27b_speed.yskn`): hidden 5120, ffn 17408, 64 layers (16 `full_attention`, 48 `linear_attention`),
vocab 248320, eps 1e-6. Engine35 hard-requires hidden 5120, 24 heads, 4 kv heads, head_dim 256, `ffn % 512 == 0` (cpp:216).

| tensor (per layer) | rows N | cols Kc | bits | G = Kc/512 | n_workers | rot_out |
|---|---|---|---|---|---|---|
| `linear_attn.in_proj_qkv` | 10240 | 5120 | 6 | 10 | 8 | yes |
| `linear_attn.in_proj_z` | 6144 | 5120 | 6 | 10 | 8 | yes |
| `linear_attn.out_proj` | 5120 | 6144 | 6 | 12 | 4 | yes |
| `self_attn.q_proj` | 12288 | 5120 | 6 | 10 | 4 | yes |
| `self_attn.k_proj`, `v_proj` | 1024 | 5120 | 6 | 10 | 4 | yes |
| `self_attn.o_proj` | 5120 | 6144 | 6 | 12 | 4 | yes |
| `mlp.gate_proj`, `up_proj` | 17408 | 5120 | 6 | 10 | 5 | yes |
| `mlp.down_proj` | 5120 | 17408 | 6 | 34 | 9 | yes |
| `lm_head` (role head) | 248320 | 5120 | 8 | 10 | 10 | no |
| `embed_tokens` (role embedding, layout `row`) | 248320 | 5120 | 6 | - | - | no |

In all of them `physical_rows == rows`. All vectors are `bf16` (`vec()` rejects anything else, cpp:170).

### 2.2 `Engine35::up(name, force_workers, src)` (engine35.cpp:136-167)

```
if (xsm_ && !force_workers && !src):                 # body tensors of the target model
    force_workers = q35_nw_override(name)            # table above (cpp:121-135)
    nw_big = max(1, min(G, ceil(want_ / (physical_rows/64))))        # cpp:141-142
t = m_.upload_linear(C.tensor(name), Mmax = 1, want_, force_workers, src)   # loader.cpp:27-60
RAR tensors rejected (cpp:158);  t.part freed (cpp:159) - part buffers are assigned later (cpp:302-312)
t.lno = lnord_build(t.frag, N, Kc, bits)             # cpp:160-162
free t.frag;  t.dev.frag = nullptr;  t.dev.lno = t.lno;  t.nw_big = nw_big
```

`TensorDev` after `up()` (kernels.hpp:12-24, loader.cpp:58 `TensorDev{t.frag, t.tile_off, t.tile_k, d.bits, t.lut8, t.tau, rows, cols, nullptr, nw}`):

| field | value |
|---|---|
| `frag` | nullptr (freed) |
| `tile_u4off`, `tile_k` | nullptr (no RAR) |
| `k_default` | `bits` (6 for body, 8 for head): bits per step = bits per 2 weights. Called `K` in the kernels |
| `lut8` | `uint16[4096]`, section 2.3 |
| `tau` | `float32[physical_rows]`, section 2.3 |
| `N` | `physical_rows` |
| `Kc` | `cols` |
| `group_worker` | nullptr -> round-robin: column group g belongs to worker `g % n_workers` |
| `n_workers` | `min(G, force_workers)` when `force_workers > 0` (loader.cpp:56) - always the case here |
| `lno` | the reordered code stream |

`n_workers` / `nw_big` (used instead of `n_workers` when `xsmc_ && !xsm_part_ && M >= 4 && nw_big > 0`, cpp:407) only decide how
the G column groups are split over CUDA blocks: worker `w` handles groups `g = w, w+nw, w+2nw, ...` (lnord.cu:190, 268).
With XSUM every worker adds exact int64 values into the same cell, so **they have no effect on the result**. Ignore them.

`DevTensor` extras (model.hpp:10-20): `signs_in int8[cols]`, `signs_out int8[physical_rows]` (nullptr without rot_out),
`m_in = rot_in.m`, `bits`, `kmax = bits`, `part` (assigned later).

### 2.3 16-bit table -> 8-bit table, `gamma`, `tau` (loader.cpp:19-25, executed on the host CPU)

```c
static void build_lut8_tau(const uint16_t* lut16, const uint16_t* s16, int rows, std::vector<uint16_t>& lut8, std::vector<float>& tau, int S) {
  float mx = 0.f; for (int i = 0; i < S * 2; ++i) mx = std::max(mx, std::fabs(h2f_host(lut16[i])));
  const float gamma = mx / 127.f; lut8.resize(S);
  for (int s = 0; s < S; ++s) { int q0 = (int)std::nearbyint(h2f_host(lut16[2 * s]) / gamma), q1 = (int)std::nearbyint(h2f_host(lut16[2 * s + 1]) / gamma);
    q0 = std::max(-127, std::min(127, q0)); q1 = std::max(-127, std::min(127, q1)); lut8[s] = (uint16_t)((uint8_t)(int8_t)q0 | ((uint16_t)(uint8_t)(int8_t)q1 << 8)); }
  tau.resize(rows); for (int r = 0; r < rows; ++r) tau[r] = h2f_host(s16[r]) * gamma;
}
```

Exactly (S = 4096, `lut16` = 8192 binary16 at `lut_off`, `s16` = `physical_rows` binary16 at `scale_off`):

```
mx     = max_i |f32(lut16[i])|                                     i = 0..8191
gamma  = RN(mx / 127.f)                                            float32
q(v)   = clamp( (int) nearbyint( RN(f32(v) / gamma) ), -127, 127 ) float32 division; nearbyint = RNE (default FP mode)
lut8[s]= (uint8)q(lut16[2s]) | (uint8)q(lut16[2s+1]) << 8          uint16: low byte = first weight of the step, high byte = second
tau[r] = RN( f32(s16[r]) * gamma )                                 float32
```

* `scale` = the file's per-row binary16 scale. It is never used alone for matrices; only `tau = scale * gamma` is kept.
* `gamma` is per tensor and is not stored anywhere except folded into `tau`.
* The value table is per tensor (one table for all rows).

### 2.4 Signs (src/signs.hpp:18-31, loader.cpp:34-36)

```
signs_in  = sign_stream_rule(rot_in.rule,  rot_in.sign_seed,  rot_in.sign_offset,  cols)            int8 +1/-1
signs_out = sign_stream_rule(rot_out.rule, rot_out.sign_seed, rot_out.sign_offset, physical_rows)   only if has_rot_out
```

`mulberry32_7919_13` (the only rule used): `s = (uint32)seed * 7919 + 13; if (!s) s = 0x9e3779b9;` then for k = 0, 1, ...:
`t = (s += 0x6d2b79f5); t = (t ^ (t >> 15)) * (t | 1); t ^= t + (t ^ (t >> 7)) * (t | 61); t ^= t >> 14;`
element k (kept when `k >= offset`) is `t < 0x80000000 ? -1 : +1`. All arithmetic uint32 wrapping.

Consequence in the shipped file: every matrix with 5120 columns has the same `signs_in` (seed 7, offset 0, first 5120 signs);
`signs_out` of a body tensor is the stream at positions `cols .. cols + rows - 1`.

### 2.5 The code stream: file layout, `repack`, `lnord_build` - the meaning does not change

Meaning (docs/FORMAT.md section 4, confirmed by the kernels): row r is cut into `S = cols/256` sequences of 256 weights; a
sequence is `128 * bits` bits = `W = 4 * bits` uint32 words, bit p = bit `p % 32` of word `p / 32`. Step j (0..127):
`e = bits * (j + 1)`, `state = bits [e-12, e)` (bits before the start of the sequence are 0; low bit of the state = oldest
bit). Weights: `w8[r][256 s + 2j] = (int8)(lut8[state] & 0xFF)`, `w8[r][256 s + 2j + 1] = (int8)(lut8[state] >> 8)`.

File layout `warp` (all matrices): word w of sequence s of row r is at word index (FORMAT.md 4.3, src/repack.cu:10-12)
```
q = s * physical_rows + r;  g = q / 32;  lane = q % 32;  c = w / 4
index = ((g * bits + c) * 32 + lane) * 4 + (w % 4)
```
File layout `row` (embedding): `index = (r * S + s) * W + w`.

Load step 1, `yskn_repack_warp_to_frag` (repack.cu:8-16, called at loader.cpp:43): pure permutation of 32-bit words into
`frag`: `dst = (((((row/64) * seqs + seq) * 8 + (row%64)/8) * bits + w/4) * 8 + row%8) * 4 + (w % 4)`. Requires `rows % 64 == 0`.

Load step 2, `lnord_build` (lnord.cu:58-88): pure permutation of 32-bit words from `frag` into `lno`. For 16-row tile
`tile16`, column group g, half `blk` (sequence `ks = 2g + blk`), lane = `gi*4 + tig` (gi 0..7, tig 0..3):
```
w[rr*K + k] = word (K*tig + k) of sequence ks of row (tile16*16 + gi + 8*rr),   rr = 0,1; k = 0..K-1      (K = bits)
out[ (((tile16*G + g)*2 + blk) * (K/2) + u) * 32 + lane ] = uint4{ w[4u], w[4u+1], w[4u+2], w[4u+3] },   u = 0..K/2-1
```
i.e. a sequence (4K words) is cut into 4 chunks of K words (= 64 weights, 32 steps) indexed by `tig`; a lane holds the chunk
`tig` of rows `gi` and `gi+8`. Size `lnord_bytes = N * Kc * K / 16` bytes (lnord.cu:79) = the size of the original stream.
Requires `N % 16 == 0`, `Kc % 512 == 0`, K in {4, 6, 8} (lnord.cu:381-383).

Decoding inside the kernel (lnord.cu:179-180, 265-266, 286-295) reproduces exactly the states of FORMAT.md: with `T = 12 - K`,
a pair of steps `p` (2K bits) and the T bits `q` that precede it, `W = q | (p << T)`, `state0 = W & 0xFFF`,
`state1 = (W >> K) & 0xFFF`. The carry-in `q` of chunk `tig > 0` is the top T bits of the last word of chunk `tig - 1`
of the same row (lnord.cu:286-288); for `tig == 0` it is 0 (start of the sequence).

**Conclusion: neither repack nor lnord_build changes any value. A port can read the `warp` words straight from the file
and use any layout.** (docs/FORMAT.md section 5 says the same about repack.)

### 2.6 Vectors: `Engine35::vec(name, n)` (engine35.cpp:168-178)

Requires `v.n == n` and `dtype == "bf16"`; uploads the raw `n` uint16 unchanged. Value = `bf2f(u)` = float32 with bits `u << 16`.

### 2.7 Scratch buffers that the gemv uses (engine35.cpp:302-312, 322-324, 342, 366), with Pmax_ = 1024, Mmax_ = 64, HM_ = 16

| buffer | type / size | content |
|---|---|---|
| `qx_` | int8 `[Pmax_ * max(F_, Q35_Z)]` = 1024 * 17408 | current gemv input (layout section 3.4) |
| `alpha_` | float `[Pmax_ * max(F_, Q35_Z)/512]` = 1024 * 34 | per-row, per-512-block scales, row stride = G of the consuming matrix |
| `part_` | allocated as `need` floats *4 bytes*; `need = max over launches of sum over segments of max(n_workers*Mmax_, Pmax_) * N` (cpp:302-306) | reinterpreted as int64 `[M][N]` per tensor for M <= 16; each tensor's `part` points into it (cpp:308-310) |
| `head_.part` | `n_workers(10) * HM_(16) * V * 4` bytes (cpp:312) | int64 `[rows <= 16][V]` |
| `ybuf_` | `part_floats_` floats (cpp:366) | float y `[M][N]` for M > 16 (`pt()`) |
| `us_` | float `[Pmax_]` | per-row scale of the last gemv (section 4) |
| `S_` | float `[Pmax_ * Q35_SST]`, `Q35_SST = 16` (q35.hpp:20) | per-row, per-512-block sum of squares of `h` (section 5) |
| `h_` | float `[Pmax_ * 5120]` | residual stream (values are bf16-representable) |

`part_` and `head_.part` are zeroed at creation (`dz35`) and by `reset()` (cpp:396-397); every consumer reads a cell and
writes 0 back (`xsum_combine`, a8_prologue.cuh:10-13), so the accumulator is always 0 when a gemv starts. A port that
overwrites instead of accumulating does not need this protocol.

---------------------------------------------------------------------------------------------------------------------

## 3. From a float vector to `qx` (int8) and `alpha` (float): the "A8" step

Every gemv input is produced by the same tail function `a8_group512_tail` (src/a8_prologue.cuh:122-133), one call per
(row, 512-block). The kernels that call it differ only in how they compute the float vector `x` before it:

| consumer gemv | producer kernel | `x[i]` fed to the tail (before the sign) | signs |
|---|---|---|---|
| q/k/v or qkv/z of layer l; gate/up; lm_head | `q35_res_norm_xsm_kernel` (q35_kernels.cu:1427-1464) | `u = RN(hn * RN(1.f + bf2f(w[i])))` - **not** divided by the RMS (section 5) | `sg_in` of the consumer |
| `o_proj` | `q35_combine_kernel` (q35_kernels.cu:1279-1362) | attention output (other area) | `Y.o.signs_in` |
| `out_proj` | `q35_gdn_out_kernel` (q35_kernels.cu:962-980) | linear-attention output (other area) | `Y.out.signs_in` |
| `down_proj` | `q35_mlp_kernel` (q35_kernels.cu:1402-1422) | `x = rbf(RN(rbf(silu(g)) * u))` (other area) | `Y.down.signs_in` |

### 3.1 Exact operation order (one row, one block b of 512; i = 512 b + t, t = 0..511)

```
1. s[t]  = RN( x[i] * (float) signs_in[i] )                    # exact (sign flip)        e.g. q35_kernels.cu:1461
2. f     = FWHT512(s)                                          # unnormalised, section 3.2
3. v[t]  = rf16( RN( f[t] * K512 ) )                           # a8_prologue.cuh:125
4. amax  = max_t |v[t]|                                        # exact (integer max of the bit patterns of |v|)
5. alpha = RN( amax / 127.f )                                  # a8_prologue.cuh:130   -> alpha[row][b]
6. r[t]  = alpha > 0 ? RN( v[t] / alpha ) : 0.f                # a8_prologue.cuh:132
7. q[t]  = clamp( rint_rn(r[t]), -127, 127 )   -> qx[row][i]   # (int8)
```

Decisive source (a8_prologue.cuh:122-133):
```c
const float v = round_f16(__fmul_rn(fwht512_v(buf[tid], buf, fw_tmp), 0.044194173824159216f));
unsigned mx = __float_as_uint(fabsf(v)); ... atomicMax(s_amax, mx);
const float al = __fdiv_rn(__uint_as_float(*s_amax), 127.f);
if (tid == 0) *alpha = al;
const float r = al > 0.f ? __fdiv_rn(v, al) : 0.f; int q = __float2int_rn(r); q = q < -127 ? -127 : (q > 127 ? 127 : q); qx[...] = (int8_t)q;
```

* The scale is **per 512-block per row** (not per vector): `alpha[row][b]`, `b = 0..G-1`.
* The 1/sqrt(512) of the input rotation is applied in step 3, on the float value before the f16 rounding. There is no
  other 1/sqrt(512) on the input side; the weights are stored already in rotated coordinates.
* Rounding is to nearest even at both places (binary16 conversion and int conversion).
* UNCERTAIN (edge only): if `|RN(f*K512)| > 65504` then `rf16` gives inf, `alpha = inf`, `r = 0 or NaN`. Not expected in
  practice; behaviour of `__float2int_rn(NaN)` is 0 on CUDA.

### 3.2 The Hadamard transform (fwht512_v, a8_prologue.cuh:87-98; identical results from fwht512_blocks :41-51)

Block size 512, natural (Sylvester) order, in place, stages in **ascending** order h = 1, 2, 4, ..., 256:
```
for h in (1, 2, 4, ..., 256):
    for every p with (p & h) == 0:
        a = x[p]; b = x[p + h]
        x[p]     = RN(a + b)
        x[p + h] = RN(a - b)
```
`fwht512_v` computes per element `q = x[p ^ h]; v = (p & h) ? (q - v) : (v + q)` - the same two values (float add is
commutative), so any implementation that keeps the stage order and the (a+b, a-b) form is bit-identical. No scaling inside.
`fwht512_blocks` does the same and then multiplies by K512.

### 3.3 The producer for the layer inputs: `q35_res_norm_xsm_kernel` (engine35.cpp:559-566, q35_kernels.cu:1427-1464)

Called by `res_norm(src, M, w, sg_in, xn_out)`; grid (5120/512 = 10 blocks, M rows), 512 threads. `mode = nmode(M)` = 0 for M <= 16.

```
if src != null:                      # after o_proj / out_proj / down_proj (N = 5120)
    acc  = part[tok][n]  (int64), then part[tok][n] = 0
    y    = RN( RN((float)acc * us[tok]) * tau[n] )                  # q35_kernels.cu:1438-1439, no r factor here
    z    = FWHT512 over the block;  z = RN( RN(z * K512) * (float)signs_out[n] )     # q35_inv_h512s, :73-78
    hn   = rbf( RN( h[tok][n] + rbf(z) ) )                          # :1441
else:                                # first res_norm of a pass: embedding
    hn   = bf2f( E[ ids[tok] * 5120 + n ] )                          # :1442 (E = decoded embedding rows, section 7.2)
h[tok][n] = hn
S[tok*16 + blk] = sum of hn^2 over the block (order in section 5.1)
u    = RN( hn * RN(1.f + bf2f(w[n])) )                               # :1459  (weight vector is "norm_offset 1": 1 + w)
if xn_out: xn_out[tok][n] = f2bf(u)                                  # :1460  (bf16, before the sign)
x    = RN( u * (float)sg_in[n] )  -> steps 2..7 of section 3.1       # :1461-1463
```

Note that the RMS factor is **not** applied before quantisation: the per-block `alpha` absorbs any common scale, so the
quantised `qx` is that of the un-normalised vector and the factor `r` is multiplied back into the gemv result by the
consumer (section 4.3, "normed").

### 3.4 Buffer layouts for M rows

Plain layout (used when `lo_now_` is false, i.e. M >= 9 in the default configuration):
```
qx[tok * Kc + c]            Kc = columns of the consuming matrix (5120, 6144 or 17408)
alpha[tok * G + g]          G = Kc / 512
```
(e.g. q35_kernels.cu:1463 `A.qx + tok * Q35_H + blk * 512`, `A.alpha + tok * (Q35_H / 512) + blk`).

LO layout (default for **M <= 8**; `lo_now_`, engine35.cpp:827): only `qx` changes, `alpha` keeps the plain layout.
For column `c = 512 blk + i` (i = 0..511) of row `tok` (0..7):
```
qx[ blk * 4096 + a8_lo_off(i, tok) ]
a8_lo_off(i, t) = ((i >> 8) << 11) | (((i >> 4) & 3) << 9) | (t << 6) | (((i >> 6) & 3) << 4) | (i & 15)     # a8_prologue.cuh:121
```
so each 512-block occupies 4096 bytes = 8 rows * 512, with the 8 rows interleaved in 16-byte pieces. Reader side:
lnord.cu:284 `P.qx + g*4096 + blk*2048 + (tok + P.lo_t0)*64 + 16*tig`, stride 512 bytes per pair of m (lnord.cu:298).
It is a pure permutation. A port may keep the plain layout for every M.

In the head launch with LO the row offset is passed as `lo_t0 = row0` and `qx` is not offset (engine35.cpp:807).

---------------------------------------------------------------------------------------------------------------------

## 4. The product

### 4.1 Dispatch (Engine35::gemv, engine35.cpp:403-430)

```c
for (DevTensor* t : ts) { ... P.seg[P.n_seg] = GemvSeg{t->dev, pt(*t, M), P.n_ctas}; ... }     // 1..3 matrices share one input
P.qx = in2_ ? qx2_ : qx_; P.alpha = in2_ ? alpha2_ : alpha_; in2_ = false;
P.lo = lo_now_ ? 1 : 0;
if (xsmc_) { P.xsum = 1; P.us_out = us_; P.xsm_part = xsm_part_;
  if (normed && M > 16) { P.xsm_S = S_; P.xsm_sst = Q35_SST; P.xsm_nb = H_ / 512; P.xsm_n = (float)H_; P.xsm_eps = eps_; } ... }
if ((pfa_ && M > Mmax_) || (xsmc_ && M > 16)) { ... if (launch_q35_g6(P, g6_, smp_p_.pm, st_)) return; launch_pfa(P, pfa_, pfa_wpc_, st_); return; }
launch_lnord(P, st_);
```

| M | kernel | `part` content |
|---|---|---|
| 1..16 | `lnord_kernel<K, MT, XSM=true, ...>` (lnord.cu:173-376) | int64 `[tok][N]` (atomicAdd), section 4.2 |
| 17..255 | `pfa_kernel<..., XSM=true>` (pfa.cu:85-321) | float `[tok][N]` in `ybuf_`: already `y` (section 4.3) |
| >= 256 | `launch_q35_g6` (q35_g6.cu:310), falls back to PFA when it returns false | same as PFA |

Gemv launches of one layer (`gemv(ts, M, normed)`): `{q,k,v}` normed (cpp:682), `{qkv,z}` normed (cpp:776), `{gate,up}` normed
(cpp:759), `{o}` / `{out}` / `{down}` not normed (via `gemv_res`, cpp:671), head not normed in the kernel but normed in its
consumer (section 7.1). The matrices of one launch share `qx`, `alpha` and therefore `us_`.

### 4.2 What is accumulated (LNORD XSUM kernel; identical in PFA and G6)

Per token (lnord.cu:52-54, 236-263):
```c
m = max over g of alpha[tok][g]  (fmaxf, start 0.f)
rmv = m > 0.f ? __fdiv_rn(16777216.f, m) : 0.f;
P.us_out[tkn] = ldexpf(m, -24);
```
Per row n, token tok, column group g (512 columns = two halves `blk` 0/1 of 256):
```c
lno_mma(d[nb], A, ...)                               // int32 d += sum over 32 columns of int8 weight * int8 qx, both halves of g
if (blk == 1) { ...
  const long long A0 = lno_xsm_A(a0, rm[nb][0]) ...  // (long long)__float2int_rn(__fmul_rn(alpha, rm))
  acc64[nb][0] += A0 * (long long)d[nb][0]; ...
  d[nb][0] = d[nb][1] = d[nb][2] = d[nb][3] = 0; }
```
and at the end (default `xsm_part == 0`), lnord.cu:353-354:
```c
atomicAdd(p64 + (size_t)t0 * N + n, (unsigned long long)acc64[nb][0]); ...
```

So:
* unit of the int32 accumulation: one **512-column group** (g) of one row for one token; `|dot| <= 512*127*127 = 8 258 048 < 2^23`.
* unit of the int64 accumulation: `acc[tok][n] = sum_g A[tok][g] * dot[tok][n][g]` over all G groups; `A <= ~2^24`,
  `|acc| < 34 * 2^47 < 2^53`. A worker contributes the groups `g % nw == worker`; all workers `atomicAdd` into the same
  cell. Integer addition is exact -> the result is independent of workers, tiles and order.
* `part` layout: `int64 part[tok * N + n]`, tok = 0..M-1 (**no worker dimension** in the default configuration).
  (`[worker][M][N]` exists only with `xsm_part_` = 1 or with XSUM off, e.g. lnord.cu:350, 357.)
* Different alphas are combined by the integer weights `A[tok][g] = rint_rn(RN(alpha[tok][g] * rm[tok]))`, i.e. each
  block's scale relative to the largest block scale of that row, in units of 2^-24.
* `us[tok] = max_g alpha[tok][g] * 2^-24` (exact) converts the integer back: `acc * us ~ sum_g alpha_g * dot_g`.

Because `|acc| < 2^53`, `(float)acc` may be computed as int64 -> double (exact) -> float (one RNE rounding); this equals
CUDA's `__ll2float_rn`.

### 4.3 From the integer to the float output in rotated coordinates

For M <= 16 (mode 0) the consumer kernel does it (`xsum_y`, a8_prologue.cuh:15; `q35_xsm_ld`/`q35_xsm_fin`, q35_kernels.cu:89-101):

```
t0 = RN( (float)acc * us[tok] )                      # xsum_y:  __fmul_rn(__ll2float_rn(acc), us)
t1 = normed ? RN( t0 * r[tok] ) : t0                 # r = rsqrt(mean(h^2) + eps), section 5
yr = RN( t1 * tau[n] )                               # tau[n] = scale[n] * gamma
```
Order is fixed: **us first, then r, then tau**. Source for the three consumer types:
```c
// not normed (residual): q35_kernels.cu:1439
buf[tid] = __fmul_rn(q35_xsm_fin(acc, A.mode, nrs.x), c_tau);
// normed (mlp gate/up, attention q/k/v, mix qkv/z): q35_kernels.cu:1412, 1023
vg = __fmul_rn(__fmul_rn(q35_xsm_fin(ag, A.xsm.mode, nrs.x), nrs.y), c_tg);
// head: misc_kernels.cu:69
x = __fmul_rn(__fmul_rn(xsum_y(xsum_combine(A.part, A.nw, A.M, A.V, row, v), nus), nrr), A.tau[v]);
```
`gamma` never appears separately (it is inside `tau`); `alpha` never appears separately (it is inside `A` and `us`).

For M > 16 the GEMM kernel itself stores `t1` as float (pfa.cu:303-306: `y0 = fmul(ll2float(acc), us); if (wr) y0 = fmul(y0, rr);`
with `wr = P.xsm_S != nullptr`, i.e. "normed"; `rr` computed at pfa.cu:161-163 exactly like section 5.2) and the consumer only
multiplies by `tau` (mode 1: `q35_xsm_fin` returns the stored float; mode 3: q35_kernels.cu:1410, 1023). Same three operations
in the same order, so M > 16 gives the same bits as M <= 16.

### 4.4 Output rotation and signs_out

For tensors with `rot_out` (all body matrices), per 512-row block of the output (`q35_inv_h512s`, q35_kernels.cu:73-78):
```
z = FWHT512(yr block)                                # section 3.2, unnormalised
y[n] = RN( RN(z[n] * K512) * (float)signs_out[n] )
```
followed by a kernel-specific rounding to bf16:
* residual (`o`, `out`, `down`): `h = rbf( RN( h + rbf(y) ) )` (q35_kernels.cu:1441);
* MLP: `g = rbf(y_gate)`, `u = rbf(y_up)` (q35_kernels.cu:1416-1417);
* attention q/k/v: `y = rbf(buf[tid])` (q35_kernels.cu:1026); mix (`qkv`, `z`): same pattern (q35_kernels.cu:823 ff., other area).

For the q_proj output N = 12288 = 24 blocks (q and the output gate), k/v 1024 = 2 blocks each; `signs_out` is indexed by the
row n of the matrix (q35_kernels.cu:1013-1015).

The head has no output rotation: `logits = yr` directly (section 7.1).

---------------------------------------------------------------------------------------------------------------------

## 5. The XSUM side channel: `us_` and `S_`

It is ON by default. Two statistics travel next to `part`:

| name | producer | consumer | meaning |
|---|---|---|---|
| `us_[tok]` | every gemv / GEMM launch (lnord.cu:219, 258; pfa.cu:158-159; gemm) | every post kernel that reads `part` in mode 0 | `max_g alpha[tok][g] * 2^-24`; float per row |
| `S_[tok*16 + b]`, b = 0..9 | `q35_res_norm_xsm_kernel` (q35_kernels.cu:1447-1458) every time `h` is updated | consumers of a "normed" gemv: `q35_mlp_kernel`, `q35_attn_post_kernel`, `q35_mix_kernel` (+ `q35_ab`), `argmax_stage1`, PFA/G6 for M > 16, `q35_xsm_xn_kernel` | sum of squares of `h` over 512-block b; only 10 of the 16 slots are used |

`us_` is overwritten by each gemv launch, so it must be consumed before the next launch (it is: gemv -> post kernel).
`S_` is valid from one `res_norm` to the next; the two "normed" gemvs that use the same `h` state read the same `S_`:
* `res_norm(after down of layer l-1 | embedding)` -> S -> `{q,k,v}` / `{qkv,z}` of layer l
* `res_norm(after o/out of layer l)` -> S -> `{gate,up}` of layer l
* `res_norm(after down of the last layer, w = final norm, sg_in = head signs)` -> S -> head.

### 5.1 `S` (sum of squares of one 512-block), exact order (q35_kernels.cu:1447-1458)

```
for t in 0..127:  a[t] = (((0.f + x[t]^2) + x[t+128]^2) + x[t+256]^2) + x[t+384]^2          # RN after every op, x^2 = RN(x*x)
for each warp w = 0..3 (t = 32w + lane): xor-shuffle tree over the 32 lanes:
    for o in (16, 8, 4, 2, 1):  a[lane] = RN( a[lane] + a[lane ^ o] )                        # all lanes simultaneously
    red[w] = a[lane 0]
S = (((0.f + red[0]) + red[1]) + red[2]) + red[3]
```
with `x` = the new `h` values `hn` of that block.

### 5.2 The norm factor `r` (q35_xsm_r, q35_kernels.cu:81-85; argmax: misc_kernels.cu:63-65)

```
s = 0.f;  for b in 0..9: s = RN(s + S[tok*16 + b])
r = rsqrtf( RN( RN(s / 5120.f) + eps ) )            eps = model eps = 1e-6f
```
`q35_rsqrt` = `rsqrtf` because `kz_` (PMATH) is 0 (q35_kernels.cu:43; with PMATH it would be `1/sqrt`). The head uses plain
`rsqrtf` unconditionally (misc_kernels.cu:65).
UNCERTAIN: CUDA `rsqrtf` is a device library function; whether `1.f / sqrtf(x)` or `sycl::rsqrt` reproduces its bits for
every argument was not determined. This is the one place in this area where bit-exactness against the CUDA engine is at risk.

---------------------------------------------------------------------------------------------------------------------

## 6. M > 1 rows

* Rows are independent in the math: each row has its own `alpha[tok][*]`, `us[tok]`, `S[tok][*]`, `acc[tok][*]`. The kernels
  only share the decoded weights. docs/TECHNOLOGY.md section 4: "computing many rows together gives the same bits as
  computing one row" - this follows from the exact integer sum.
* `qx`: plain `[tok][Kc]` for M >= 9; LO-interleaved for M <= 8 (section 3.4). `alpha`: `[tok][G]`. `part`: int64 `[tok][N]`
  for M <= 16, float `[tok][N]` in `ybuf_` for M > 16. `us_[tok]`, `S_[tok*16 + b]`, `h_[tok][5120]`.
* Limits:
  * LNORD with XSUM: M <= 16 (lnord.cu:379). `xsm_part` would additionally need `n_workers <= 24` (engine35.cpp:415; not used).
  * `HM_ = 16`: rows per head launch; `HEAD_ALL` loops over slices of 16 rows (engine35.cpp:837), `head_.part`, `logits_`
    and the argmax/top-k scratch are sized for 16 rows (cpp:312, 336-338).
  * `Mmax_ = 64`: attention scratch (`pm_`, `pl_`, `pacc_`), `Y.pmix`, `Y.pab`; passes with more rows slice the attention
    (cpp:711, 722-733) and use the `*_big_` buffers (cpp:771).
  * `Pmax_ = PM_ = 1024` (default env): rows of all per-row scratch buffers and of one prefill chunk.
  * LO needs M <= 8 (8 row slots per 512-block). In `head_slice` with LO, `row0 + rows <= 8`.
* Kernel-shape choices by M (`lno_cfg_of`, lnord.cu:416: table `{0,1,2,1,2,2,2,2,2}` for M = 0..8; MT = 8 for M <= 8, 16 for
  M <= 16) and `nw_big` for M >= 4 are performance only.
* Verify passes of the default speculation use at most 16 rows (`gamma = 7`, `gamma_long = 15`, runtime.py:54-56).
  UNCERTAIN: whether a verify/decode pass can ever have 17..64 rows in the default configuration was not traced (specx.cpp is
  outside this area). If it can, it goes through PFA with identical results (section 4.3).

---------------------------------------------------------------------------------------------------------------------

## 7. LM head and embedding

### 7.1 LM head (`head_slice`, engine35.cpp:803-823)

Differences from a body matrix:

| item | head |
|---|---|
| load | `head_ = up("lm_head", hw = cols/512 = 10)` (cpp:268-269): same `upload_linear` + `lnord_build`; `bits = 8` (K = 8, 4 bits per weight, state = 12 bits ending at `8 (j+1)`); `n_workers = 10`, `nw_big = 0` |
| table / tau | same 8-bit rounding as section 2.3 (`lut8`, `tau[248320]`) |
| output rotation | none (`rot_out = null` -> `signs_out = nullptr`) |
| input | `qx_`/`alpha_` produced by the last `res_norm` with `w = fnorm_` (vector `norm`) and `sg_in = head_.signs_in` (cpp:799) |
| launch | `launch_lnord` directly, `P.M = rows <= 16`, `P.qx = qx_ + row0*5120`, `P.alpha = alpha_ + row0*10`; with LO: `P.qx = qx_`, `P.lo_t0 = row0` (cpp:804-809). `P.xsum = 1`, `P.us_out = us_` -> `us_[0..rows-1]` are written **relative to the slice** |
| part | `head_.part`: int64 `[rows][V]` |
| consumer | `argmax_stage1<true>` (misc_kernels.cu:57-89) with `AA.us = us_`, `AA.rr = S_ + row0*16`, `xsm_nb = 10`, `xsm_stride = 16`, `xsm_n = 5120.f`, `xsm_eps = eps_` (cpp:810-811) |

```
r      = rsqrtf( RN( RN( (sum_{b=0..9} S[(row0+row)*16 + b]) / 5120.f ) + eps ) )      # sequential sum from 0.f
logit[row][v] = RN( RN( RN( (float)acc[row][v] * us[row] ) * r ) * tau[v] )            # misc_kernels.cu:69
```
`logits_` is float `[16][V]`, row index relative to the slice. Greedy argmax: largest value, ties -> smallest index
(misc_kernels.cu:73, 76). `HEAD_LAST` runs one row `row0 = M-1` (cpp:836); `HEAD_ALL` runs slices of `HM_` rows (cpp:837).
Sampling (`smp_`), top-k and NLL read `logits_` afterwards (cpp:814-822; other area).

### 7.2 Embedding (role `embedding`, layout `row`, 16-bit table)

Load (engine35.cpp:272-298): the tensor must be `role embedding, layout row, rows = V, cols = 5120, physical_rows = V, l = 12,
start zero, no rot_out, rot_in H512, no rar` (cpp:275-277). Uploaded **unchanged**:
`emb_codes_` = the raw word stream (`V * 20 * 4*bits` uint32, row layout), `emb_lut_` = 8192 binary16 (the 16-bit table,
**no 8-bit rounding, no gamma**), `emb_sc_` = V binary16 scales, `emb_sg_` = `signs_in` (5120 int8), `emb_K_ = bits` (6).
The shipped file has no `embed_tokens.res` / `.res2` tensors, so `emb2_codes_` and `emb3_codes_` are null.

Use (`emb_src`, engine35.cpp:576-589, called from `res_norm(nullptr, ...)`): `launch_q35_embed` decodes the M rows
`ids[0..M-1]` into `emb_rows_` (bf16 `[M][5120]`) and returns `gids = emb_iota_` (0, 1, 2, ...), so the res_norm kernel reads
row `tok`. Kernel `q35_embed_blk_kernel` (q35_kernels.cu:1527-1556), per row and 512-block b:

```
id = ids[row];  sc = f32(emb_sc[id]);  wps = 4*K;  rowwords = codes + id * 20 * wps
for sequence q in (2b, 2b+1), step j in 0..127:
    e = K*(j+1)
    s = e >= 12 ? bits[e-12, e) of the sequence : (bits[0, e) << (12 - e))          # q35_read_window, :1496-1500, :1533
    x[(q-2b)*256 + 2j]   = RN( f32(lut16[2s])   * sc )
    x[(q-2b)*256 + 2j+1] = RN( f32(lut16[2s+1]) * sc )
x = FWHT512(x)                                                                       # ascending h, (a+b, a-b), :1548-1551
E[row][512b + i] = f2bf( RN( RN(x[i] * K512) * (float)signs_in[512b + i] ) )         # :1553-1554
```
Then `hn = bf2f(E[row][n])` becomes `h` (q35_kernels.cu:1442) and continues as in section 3.3 with layer 0's `ln1` and
`signs_in`. So the embedding is: 16-bit table value * row scale (float), input-side rotation only, result rounded to bf16.

Differences from body matrices, in one list: table stays 16-bit float (no `lut8`, no `tau`); the scale is the raw binary16
row scale; codes are in `row` layout and are not repacked; only a row lookup is done (no product, no qx/alpha, no XSUM);
the rotation applied is the *input*-side one (H512 over the 5120 columns then `signs_in`), which for a row lookup yields the
row of `W` in model coordinates (FORMAT.md 4.6: `W = M * Hn * D_in`).

---------------------------------------------------------------------------------------------------------------------

## 8. Reference pseudo-code for the whole default gemv (M <= 16 semantics, valid for any M)

```
# load (once per tensor)
lut8[4096] (2 x int8), tau[N] f32                      <- section 2.3
signs_in[Kc], signs_out[N] (body only)                 <- section 2.4
codes: any layout; w8(r, c) defined in section 2.5

# input: x[M][Kc] float32 (already multiplied by nothing; the producer applies signs_in)
for tok, for b in 0..G-1:
    s = x[tok][512b .. 512b+511] * signs_in[...]
    f = FWHT512(s);  v = rf16(RN(f * K512))
    alpha[tok][b] = RN(max|v| / 127.f)
    qx[tok][512b+t] = clamp(rint_rn(alpha > 0 ? RN(v[t] / alpha) : 0), -127, 127)

# product
for tok:
    m = max_b alpha[tok][b];  rm = m > 0 ? RN(16777216.f / m) : 0;  us[tok] = ldexpf(m, -24)
    A[b] = (int64) rint_rn(RN(alpha[tok][b] * rm))
    for n: acc[tok][n] = sum_b A[b] * sum_{c in block b} (int32)w8(n, c) * qx[tok][c]          # int64

# output
for tok, n:  yr = RN( RN( RN((float)acc[tok][n] * us[tok]) [* r[tok]] ) * tau[n] )             # r only if "normed"
body: per 512 rows:  y = RN( RN(FWHT512(yr) * K512) * signs_out )   -> then the consumer's rbf()
head: logits = yr
```

---------------------------------------------------------------------------------------------------------------------

## 9. Uncertainties

1. `rsqrtf` (section 5.2): CUDA device-library function used for the norm factor `r` in every "normed" consumer and in the
   head (PMATH off by default). Its exact rounding is not specified in the source; bit-exact reproduction on another GPU was
   not determined. Everything else in this area is integer arithmetic or single IEEE operations.
2. f16 overflow in the A8 step (section 3.1): behaviour for `|v| > 65504` (inf) not relevant in practice, not analysed further.
3. Whether default speculation ever issues a pass with 17..64 rows (section 6): not traced; the PFA path gives the same bits.
4. G6 (M >= 256 prefill, q35_g6.cu): only the scale/finish formulas were checked (q35_g6.cu:140-146, 217, 265-269: same
   `rm`, `us`, `A`, `ll2float * us [* rr]`); the int8 pre-decode and tiling were not read line by line.
5. `silu`, attention and linear-attention math that produce the inputs of `o_proj`, `out_proj`, `down_proj` (section 3 table)
   are outside this area; only the common tail (`a8_group512_tail`) is specified here.
6. The drafter (`bdraft2`, role `draft`, bits 8, no rot_out) is loaded through the same `up()` (with `src = &bdr_.c`, so
   without the `YSKN_Q35_NW` override and with `nw_big = 0`); its gemv call sites (`bdr_gemv`) were not read.
