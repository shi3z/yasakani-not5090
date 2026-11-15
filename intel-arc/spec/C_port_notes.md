# C. Full attention + KV5 cache: SYCL port notes (`src/attn.hpp`)

Companion of `spec/C_attention.md` (math) and `ref/core.py` class `Attn` (executable reference, the bit-exact target).

## Interface
```
yx::Attn A(E, layer, max_ctx);            // or Attn(E, layer, attn_shared(E, max_ctx)); 16 layers share one AttnShared
yx::attn_forward(E, A, M, pos0);          // chain pass, rows at slots pos0..pos0+M-1, M <= MRMAX (16)
E.res_norm(&A.o, M, ln2, gate.sg_in, false);
```
* In: `E.qx / E.alpha / E.S` (and `E.us/E.Aw` when `E.aw_ready`) from the preceding `res_norm`. Out: `A.o.part`, `E.us`.
* `q.part = E.part`, `k.part = E.part + 16*12288`, `v.part = k.part + 16*1024`, `o.part = E.part` (q/k/v are consumed before o).
* The KV profile is read from `$YX_KV_PROFILE`, default `models/speed/kvq_u5.bin` (`attn_default_profile`).
* Rows of a rejected pass need no undo: the next pass overwrites their slots (spec C 5.4). Tree passes (`wk` depth table)
  are NOT implemented: depth = row index only.
* `E.fast` (default) runs the ESIMD kernels (`namespace yx::ak`), `YX_SCALAR=1` the scalar kernels kept in
  `attn_forward`; both are bit-exact against the reference. `yx::attn_prof` (tests) records per-stage time stamps.

## Device data
| Buffer | Layout |
|---|---|
| `Attn::kp`, `vp` | `[4 kv heads][max_ctx][40]` u32, the reference payload words (MSB-first 5-bit fields), + 64 words slack |
| `Attn::kps`, `vps`, `vs` | `[4][max_ctx]` f32 (+ 64 slack: tiles load 32 scales at once) |
| `Attn::v8` | `[4][max_ctx][256]` int8, only layers 43, 59, 63 (ordinals 10, 14, 15) |
| `Attn::gf` | `[8 groups][5][256]` f32: encoder codebook, decode table (BF16 values), mean, U0, U1; groups 0-3 = K heads, 4-7 = V heads |
| `Attn::gci`, `gmeta` | `[8][256]` int codebook; `[8][4]` = guard, m, has_mu, 0 |
| `AttnShared::rope` | `[max_ctx][64]` f32: cos[32], sin[32], BF16 values, made on the HOST (`rope_table_host` = `pm_sincos_f` in double) |
| `AttnShared::q16, gate, y, y2` | `[16][24][256]` f32 (`y`, `y2` only for tests) |
| `enc_r, enc_c, enc_e, enc_st` | encoder scratch: residual `[16][8][256]`, (c0, c1, rms, guard flag) `[16][8][4]`, tree32 errors `[16][8][9][8]`, states `[16][8][9][256]` u8 |
| `pm, pl, pacc` | split partials `[16][42][24]`, `[16][42][24][256]` (16.5 MB) |
| `vxi, vbp` | ESIMD trellis scratch per (row, group, scale, chain): xi `[64]` int, back pointers `[64][16]` u8 |

## Kernels (5 launches + 2 GEMM groups)
1. `post` (row x 28 blocks): projection -> y; q: gate, RMS norm, RoPE, rotation -> `q16`; k/v: rotation, INT8 row,
   mean / low-rank removal, rms -> `enc_r/enc_c` (guard row: flag, `enc_r` = INT8 row value).
2. `viterbi` (row x 8 groups x 9 scales x 4 chains): the plain recursion of spec C 3.3. ESIMD: 256 costs in 16 registers
   (state = 16 v + lane), per lane first-min over v, then the two parities combined with the smallest-j tie rule.
3. `select` (row x 8 groups, scalar): first minimum over the 9 scales, pack 40 words (or `store8(x')` / guard `store8(y)`).
4. `attend<NG>` (row x 24/NG head groups x 42 splits; NG = 1 for M <= 2, else 3; `YX_ATTN_NG` overrides): tiles of 32 slots,
   keys decoded on the fly (2 payload gathers + 1 table gather per 16 values), own key, `l * mu`.
5. `combine` (row x 12 blocks, one work-group per row): combine, inverse rotation, gate, a8 (`fx::v_a8_tail`, G = 12),
   `fx::v_scales` (sets `E.aw_ready`), then `E.gemm_group({&A.o}, M)`.

## Deviations introduced in the reference (`ref/core.py`), all in `Attn` / `exp2f`
The device has no usable fp64, so three float64 spots of the reference were replaced by fixed float32 definitions:
1. `exp2f(x) = pm_expf(f32(x * LN2_F))`, `LN2_F = 0x3f317218` (was `f32(pm_exp_d(double(x) * ln2))`).
   `exp2f(0) == 1`, `exp2f(-inf) == 0` exactly; max relative error vs 2^x on [-40, 3]: 1.1e-6.
2. `_dots` (prefix-key dot product, CUDA: 16 `mma` steps, order unspecified): chunks of 16 elements in order;
   the 16 exact products of a chunk are reduced by the XOR tree `for o in (8,4,2,1): a[i] += a[i^o]` -> `a[0]`
   (= pairwise halving), then `acc = acc + a[0]`.
3. V accumulation of a tile (CUDA: two `mma` steps of 16 keys): after `o = o * c`, the keys of the tile in slot order,
   `o[d] = o[d] + bf16(p_j * vs_j) * V_j[d]`; masked slots are skipped.
The own-key path, combine and everything else are unchanged. Not changed but relevant: empty splits are skipped in
combine (weight 0 adds +0; O and L can never be -0).

Oracle check (`python3 ref/run_tf.py a 3 7 63`, CUDA captures `work/oracle/a.npz`, 12-row prefill):

| Layer (teacher-forced) | before | after |
|---|---|---|
| 3 | rel 1.414e-03, exact-equal 0.5102, rows exact 100000000000 | identical |
| 7 | rel 7.450e-04, exact-equal 0.7745, rows exact 101000011101 | identical |
| 63 | rel 1.670e-03, exact-equal 0.8573, rows exact 111101010101 | identical |

Other dot orders tried against the oracle (layer 3 / 7 / 63 rel): 16 lanes accumulated over the chunks then tree
1.414e-3 / 8.108e-4 / 1.367e-3; plain sequential 6.483e-4 / 6.835e-4 / 1.367e-3; chunk-sequential and a compensated
(TwoSum) chunk sum = the chosen one. The differences are single BF16 flips in single rows, i.e. noise; the chunk tree
was kept because it reproduces the previous numbers on all three layers and keeps the `mma` step structure.
Deviation 3 is not exercised by this oracle (at most one key per tile at 12 rows).

## Tests
```
./build.sh tests/attn_test
python3 ref/run.py 4 a                       # reference free run, writes work/ref_h_{0..3}.npy
tests/attn_regress.sh                        # all cases; every line must read RESULT ALL BIT-EXACT (gen: recreate inputs)
./tests/attn_test chain c12 3 0 12   && python3 tests/attn_cmp.py chain c12 3 0 12
./tests/attn_test unit  v43 43 0 16 7 1 ...  && python3 tests/attn_cmp.py unit v43 43 0 16 7 1 ...
YX_NODUMP=1 YX_TIME=30 ./tests/attn_test unit v3 3 0 11                      # timing at pos0 = 11
YX_NODUMP=1 YX_TIME=30 ./tests/attn_test unit v3 3 0 $(printf '16 %.0s' {1..64})   # timing at pos0 = 1024
```
`chain`: layers 0..3 on the device from `work/ids.bin`; `unit`: one attention layer fed with hidden rows
from `<tag>_in.bin` (`h` = after the attention residual); `pos_start > 0` loads a random cache prefix written by
`attn_cmp.py gen`. Compared bit-for-bit: RoPE table (all max_ctx rows), layer input, `q16`, gate, cache
(`kp/kps/vp/vps` or `v8/vs`), `y`, `y2`, `h`. Files under `work/attn/`.

Cases that pass bit-exactly (ESIMD kernels): 12 prompt rows through layers 0..3 in one pass (also equal to
`ref_h_3.npy`) and as 5+1+2+4 passes (dumps identical to the one-pass run); layers 43 / 59 / 63 (INT8 V: guard row,
low-rank terms, mean) with 12 rows in 7+1+4; layers 3 / 43 / 63 with 100 rows in mixed passes (tiles and splits with
several keys); random 4085- and 4090-row prefixes crossing len = 4096 (31 -> 42 splits) on layers 3 and 43; a
1000-row prefix on layer 51 (K group with a mean) and 997 rows on layer 59.

## Timings (B60, one attention layer = `attn_forward`, layer 3, wall time with `q.wait()`, min of 30)
| Case | total | qkv GEMM | post | viterbi | select | attend | combine + o GEMM |
|---|---|---|---|---|---|---|---|
| layer 3, M=1, pos0=11 | 0.338 ms | 0.155 | 0.030 | 0.041 | 0.014 | 0.022 | 0.108 |
| layer 3, M=8, pos0=11 | 0.444 ms | 0.163 | 0.035 | 0.101 | 0.017 | 0.045 | 0.121 |
| layer 3, M=1, pos0=1024 | 0.485 ms | 0.155 | 0.031 | 0.042 | 0.015 | 0.140 | 0.134 |
| layer 3, M=8, pos0=1024 | 0.914 ms | 0.161 | 0.035 | 0.101 | 0.017 | 0.489 | 0.142 |
| layer 43 (INT8 V), M=1, pos0=1024 | 0.542 ms | 0.154 | 0.032 | 0.041 | 0.090 | 0.123 | 0.134 |
| layer 43 (INT8 V), M=8, pos0=1024 | 0.941 ms | 0.163 | 0.035 | 0.101 | 0.107 | 0.426 | 0.143 |
| scalar kernels (`YX_SCALAR=1`), layer 3, M=1, pos0=1024 | 13.9 ms | 0.163 | 1.240 | 2.592 | 0.020 | 7.657 | 2.277 |

Stage columns: min over 30 runs with a `q.wait()` after every stage (they add up to slightly more than the total).
Context 1024 was reached by 64 passes of 16 rows (100 synthetic rows used cyclically). Heads per attend thread at
context 1024 (attend stage, M=1 / M=8): NG=1 0.141 / 0.900 ms, NG=2 0.148 / 0.574, NG=3 0.161 / 0.484, NG=6 0.202 / 0.492.

## Open issues
* No tree passes (`wk` depth table, spec C 5.3) and no `lfrow_promote` row copy; M <= 16 rows per call (prefill must be
  chunked by the caller; a 64-row slice path does not exist).
* GEMM launches dominate at short context (q/k/v: 3 kernels, ~0.3 ms synced). `attend` is bound by gathers (96 per key
  and thread): candidates are the pair LUT of the CUDA engine (13-bit window -> two values), tables in SLM, one 32-lane
  payload gather instead of two, and reusing decoded keys across the rows of a pass.
* `viterbi` at M = 16 is 4608 threads of ~40 us of work each; `select` is still scalar (15 us, 0.1 ms for INT8-V layers).
* `pacc` zero-skips rely on `pm <= -1e29` marking empty splits; `pacc` of an empty split is stale memory by design.
* The first JIT of `tests/attn_test` takes ~75 s (scalar + ESIMD kernels); `SYCL_CACHE_PERSISTENT=1` avoids it.
* `ref_h_<l>.npy` for l > 3 under `work` predate the reference change (only used as synthetic inputs).
