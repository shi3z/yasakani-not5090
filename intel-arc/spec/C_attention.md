# C. Full attention and its KV cache (default configuration)

Scope: the 16 full-attention layers of Qwen3.8-27B (`arch qwen3_5`) as the CUDA engine runs them with the shipped
defaults. All paths are relative to `../`. Model layers that are full attention:
3, 7, 11, ..., 63 (`layer_types`, every 4th). "Layer ordinal" `L` below means 0..15 over those layers.

Constants (`src/q35.hpp:13`): `H=5120, NH=24, KVH=4, HD=256, RD=64`; `G = NH/KVH = 6`; `eps = 1e-6` (`rms_norm_eps`).

Notation: `rbf(x)` = round f32 to bf16 (nearest-even) and back to f32. `f16r(x)` = round to IEEE half and back.
`rn()` = round to nearest, ties to even. Every `a*b+c` is a separate multiply and add (`--fmad=false`).
`tree32(v[0..31])`: `for o in (16,8,4,2,1): v[i] = v[i] + v[i^o]` (all i at once); result `v[0]`.
`S256(v)` = `t=0; for w in 0..7: t = t + tree32(v[32w .. 32w+31])`.

Verification status. Section 3 (rotation, INT8 row, 5-bit trellis encode, packing, state decode) was re-implemented in
NumPy from this text and compared with the CUDA library through its probe exports (`yskn_kvx_probe`,
`yskn_tbp_pack_unpack`, `yskn_tb_probe`): 28 rows over 14 groups, payload bits, scale, and reconstructed floats were all
bit-identical. Sections 2, 4, 5 are from reading the source only; nothing in them was executed.

---------------------------------------------------------------------------------------------------------------------

## 1. Which path runs

### 1.1 Effective settings

The Python package sets environment variables before the library loads (`python/yasakani/runtime.py:27-42`); they
differ from the C++ defaults, and the log line you quoted ("staging 4096 rows") confirms they are in effect.

| Setting | C++ default | Shipped value | Meaning |
|---|---|---|---|
| `YSKN_KVX_MODE` (compile) | 0 | 3 | Hadamard-rotated INT8 KV + trellis profile (`src/kvx_codec.cuh:8-18`) |
| `kv8_` `YSKN_Q35_KV8` | 1 | 1 | INT8 row + f32 scale containers (`engine35.hpp:267`) |
| `tbcap_` `YSKN_TB_CAP` | 0 | **1** | "capacity mode": store 5-bit packed rows instead of INT8 (`engine35.cpp:207`) |
| `tbcap_kb_` | 4 | **5** | bits per value, majority vote over the profile's groups (`kvq_atxpk.hpp:73-84`) |
| `atx_` `YSKN_Q35_ATX` | 1 | 1 | tensor-core attention family ("ATX") (`engine35.hpp:281`) |
| `atx_splits_` | 42 | 42 | number of key-range splits when the visible length is >= `atx_long_` |
| `atx_short_` | 31 | 31 | number of splits when the visible length is < `atx_long_` |
| `atx_long_` | 4096 | 4096 | threshold between the two (`engine35.hpp:284`) |
| `atx_vsp_` `YSKN_ATX_VSP` | 3 | 7 | kernel-shape bitmask for the **unpacked** INT8 ATX path only (`q35_atx.cu:822`); unused here |
| `YSKN_ATX_PKV` | 3 | 3 | packed-attention kernel variant (`q35_atxpk.cu:683`) |
| `YSKN_ATX_PK3_PLUT` | 0 | 1 | pair lookup table in the M<=2 kernel; speed only |
| `YSKN_TB_VIT2` | 1 | 1 | faster Viterbi kernel; same bits as the plain one |
| `fa_`, `fa_splits_`, `splits_` | 1, 31, 32 | same | only used when `atx_ == 0`; dead in this build (see below) |
| `Mmax_` | 64 | 64 | rows per attention launch; larger passes are sliced |
| `Pmax_`/`PM_` `YSKN_Q35_PMAX` | 512 | 1024 | rows per prefill sub-block |
| `YSKN_Q35_LAYER_ROWS` | 0 | 4096 | layer-major prefill chunk; also sets staging rows |
| `kz_` `YSKN_Q35_PMATH` | 0 | 0 | portable-math mode; off |
| `HM_` | 16 | 16 | max rows of a verify pass |

`KVX_MODE == 3` makes ATX mandatory: `engine35.cpp:679` `if (!atx_ || kz_) throw ... "Rotated INT8 requires ATX attention"`.

### 1.2 Dispatch (`Engine35::attn_block`, `engine35.cpp:675-737`)

```
gemv(q,k,v)                                   # integer partial sums (common part)
attn_post  (or wk_attn_post if wk_now_)       # -> q16, gate16, INT8 K/V rows       (sec. 2, 3.1-3.2)
tb_requant                                    # INT8 rows -> 5-bit packed rows       (sec. 3.3)   engine35.cpp:704
if M <= 64:  atx_pk(M rows) ; combine         # engine35.cpp:711-721
else:        for each slice of 64 rows: atx_pk(slice, d_pos = base+s) ; combine     # engine35.cpp:722-733
gemv_res(o_proj)
```

`cap_ord >= 0` for every layer because K is packed in all 16 (`engine35.cpp:677`), so **every** attention launch goes to
`launch_q35_atx_pk` (`engine35.cpp:716, 729`). Inside it (`q35_atxpk.cu:702-742`), with `PKV=3`, `kb=5`:

| Rows in the launch | Kernel | File |
|---|---|---|
| M <= 2 (`YSKN_ATX_PK3_MMAX`=2) | `q35_atxpk3_kernel<5, VPK, 1,4, NST=2, SKEW, 4, QK8=false, PLUT=true>` | `q35_atxpk3.cuh:488-515` |
| 3 <= M <= 8 | `q35_atxpk4_kernel<5, VPK, 3, 4, false>` | `q35_atxpk4.cuh:431-435` |
| 9 <= M <= 64 | `q35_atxpk4_kernel<5, VPK, 6, 2, false>` | same |

`VPK` = whether V of that layer is packed (13 layers) or INT8 (layer ordinals 10, 14, 15).

* **Decode** (M = 1, or a verify pass of up to 16 rows): one launch, pk3 for M<=2, pk4 above.
* **Prefill**: layer-major (`engine35.cpp:1107-1162`), sub-blocks of up to 1024 rows per layer. `attn_post` and the
  pack step run on the whole sub-block first, then attention runs in slices of 64 rows (pk4 `<6,2>`, remainder of 1-2
  rows through pk3). The attention maths is the same as decode.

All these kernels compute the same function (sec. 4); they differ in thread layout only.

### 1.3 Names

* `splits` (`Q35AttnArgs.splits`): the key range `[0, len)` of a row is cut into `S` equal consecutive pieces; one CTA
  per (row group, split, KV head) produces a partial softmax state (`pm`, `pl`, `pacc`); `combine` merges them. It is a
  parallelisation device. It changes results only in float rounding (sec. 4.5).
* `atx_short_/atx_long_`: `S = 31` if `len < 4096` else `42` (`q35_atxpk.cu:403`, `q35_kernels.cu:1286`). `A.splits`
  stays 42 as the **stride** of the `pm/pl/pacc` arrays.
* `vsp`: see table; ignore.
* `wk`: optional 48-byte row table on the device (`d_wk_`). `wk[tok]` (bytes 0..15) is the **depth** of pass row `tok`.
  Without it depth = tok. Used for tree-shaped verify passes (sec. 5.3). `wk_now_` is true only when option 6
  (`opt_lfrow_`) is on, the pass is not a commit pass, M <= 16 and all rows get a head (`engine35.hpp:364`).
  `specx.cpp:195` turns option 6 on for a generation whenever the block drafter and HCAP are on (`YSKN_HCAP` defaults to 1,
  `engine35.cpp:1693`), so in default generation verify passes do carry a table; it is the identity table
  (`wk_plain_table`, `engine35.cpp:871`) unless the pass has leaf rows.
* `early`: 1 on the M<=64 path. A launch-overlap flag (keys below `base` are loaded before the previous kernel has
  finished). No effect on results.

### 1.4 What you can ignore

* `q35_kernels.cu` `q35_attention_kernel` and `q35_softmax*` (`:1141-1276`): plain path, `atx_==0` only.
* `src/q35_fa.cu`: prefill attention for `atx_==0`.
* `src/q35_atx.cu` (all three kernels): ATX on unpacked INT8/BF16/FP16 KV. Reached only when a layer has neither stream
  packed, i.e. with `YSKN_TB_CAP=0`. Same formulas as sec. 4 with K and V both read as INT8.
* `q35_atxpk.cu` `q35_atxpk_kernel` (variant 1, k=4 only) and `q35_atxpk_vsp_kernel` (variant 0).
* In pk3/pk4: the `QK8` branches (`YSKN_ATX_PK3_QK8`, INT8 query), the non-PLUT/`KB==4` branches.
* `src/q35_kz.cu`, `kz_common.cuh`, `q35_kz_combine_kernel`: PMATH mode.
* `src/attention_gqa.cu`, `src/attention_kv4q.cu`: other architectures (28 heads / head dim 128).
* `src/pfa.cu`: prefill GEMV (weights), not attention. `src/v2_decode.cuh`: weight-code reader for GEMM.
* `src/hcap_tables.inc`: tables of the HCAP leaf chooser in the drafter (`q35_bdraft.cu`); it only produces the `wk`
  table.
* `src/kvx_probe.cu`: test/bench exports. Not on the inference path, but they are usable oracles (sec. 8).
* `q35_visual_rope`, `kv_fp16`, BF16 KV (`kc/vc`).

---------------------------------------------------------------------------------------------------------------------

## 2. attn_post: partial sums -> q, gate, k, v

Kernels: `q35_attn_post_kernel` (`q35_kernels.cu:1009-1060`), `wk_attn_post_kernel` (`lfrow_kernels.cuh:154-207`, the
same plus a depth table), `q35_attn_post_pf_kernel` (`q35_kernels.cu:610-690`, used when M > 64 with
`YSKN_Q35_PFALT=1` and xsm mode 3, `:1062`). The three perform the same float operations in the same order.

### 2.1 Projection outputs and blocks

* `q_proj` has **N = 2*24*256 = 12288** outputs, `k_proj` and `v_proj` have 1024 each (`:1013-1015`).
* Work is in blocks of 512 outputs. Blocks 0..23 = q (block b = head b), 24..25 = k (2 KV heads each), 26..27 = v.
* In q block `b`: outputs `[0,256)` are the query of head `b`, outputs `[256,512)` are the **gate pre-activation** of
  head `b` (`:1038` `if (blk < Q35_NH && tid >= Q35_HD) A.gate16[...] = f2bf(y)`). So the gate is the upper half of each
  512-wide chunk of q_proj.

### 2.2 Common part (reference only)

For output index `n` of a projection, row `tok`:

```
raw[n]  = mode 3 (M > 16):  the f32 stored in part[]                              (already input-normed by the GEMV)
          mode 0 (M <= 16): (float(acc_int64[n]) * us[tok]) * r_tok              r_tok = rsqrt(sum(S[tok][0..9])/5120 + eps)
t[n]    = raw[n] * tau[n]
z       = FWHT512 over each 512-block (butterflies h = 1,2,...,256; (a,b) -> (a+b, a-b); lower index gets the sum)
y[n]    = rbf( (z[n] * 0.044194173824159216f) * signs_out[n] )
```

(`:1021-1026`, `q35_inv_h512s` `:73-78`, `fwht512_v` `a8_prologue.cuh:87-98`.) Defined by the GEMV/common spec.
UNCERTAIN: I did not check that mode 0 and mode 3 give bit-identical `y` for the same token; the KV5 design text says
rows written in a 1-row pass and in a multi-row pass are the same bits, and the encode from `y` onward is per-row.

### 2.3 V rows

`v = y` (no norm, no RoPE). Then rotation and store (sec. 3.1, 3.2). `:1027-1034`.

### 2.4 q and k: RMS norm per head

Per head (256 values `y[0..255]`; the gate half is not used):

```
tot = S256(y*y)                                   # tree32 per 32 values, then sequential over the 8 chunks  (:1039-1044)
r   = rsqrtf(tot / 256.0f + eps)                  # (:1045)
xn[d] = rbf( (y[d] * r) * (1.0f + bf2f(w[d])) )   # w = q_norm / k_norm vector, 256 BF16, shared by all heads (:1046)
```

The stored norm weight is `weight - 1`; the kernel adds 1. `w` is `layers.N.self_attn.q_norm` / `.k_norm`
(`engine35.cpp:241`).

### 2.5 RoPE

Table (`q35_rope_table_kernel`, `:1091-1105`), `i in 0..31`, position `t`:

```
inv_freq[i] = the 32 f32 constants at q35_kernels.cu:1091-1094 (bit patterns; = 1e7^(-i/32), [0] = 1.0)
f = (float)t * inv_freq[i]                        # f32 multiply
rope[t][i] = ( rbf(cosf(f)), rbf(sinf(f)) )
```

Applied to dims 0..63 only, pairing `d` with `d+32` (half-split, not interleaved) (`:1049-1053`):

```
for d in 0..31:   (c,s) = rope[pos][d]
  out[d]    = rbf( rbf(xn[d]    * c) + rbf( (-xn[d+32]) * s) )
  out[d+32] = rbf( rbf(xn[d+32] * c) + rbf(   xn[d]     * s) )
out[d] = xn[d]  for d >= 64
```

`pos = *d_pos + tok` in the plain kernels. With a `wk` table the RoPE position is `rpos = *d_pos + wk[tok]`
(`lfrow_kernels.cuh:167,197`); the storage slot stays `*d_pos + tok`.

UNCERTAIN (last-bit): CUDA `cosf/sinf/rsqrtf` are not correctly rounded. The following `rbf` hides almost all of it,
but a different libm can flip an occasional BF16 rounding. Copy the 32 `inv_freq` bit patterns; compute cos/sin in
double and round.

### 2.6 Outputs

```
q:  q16[tok][head][d]    = bf16( rotate(out)[d] )           # rotation of sec. 3.1; BF16, 2 bytes   (:1054-1057)
g:  gate16[tok][head][d] = bf16( y[256+d] )                 # not normed, not rotated               (:1038)
k:  K row (kvh, slot)    = store8( rotate(out) )            # sec. 3.2                              (:1058-1059)
v:  V row (kvh, slot)    = store8( rotate(y) )              #                                       (:1030-1034)
```

Queries stay BF16 and are never quantised further.

---------------------------------------------------------------------------------------------------------------------

## 3. KV cache format

Pipeline for one K or V row (256 floats, one KV head, one position):

```
x  --rotate-->  xr  --store8-->  (q8[256], sc)  --y = q8*sc-->  trellis encode (profile group g)
                                                   |-- stream packed:   40 words of 5-bit fields + f32 scale    (kp/kps, vp/vps)
                                                   '-- stream unpacked: x' = decoded floats -> store8 again     (v8/vs)
```

### 3.1 Rotation (`kvx_rotate`, `src/kvx_codec.cuh:28-49`; warp form `:74-90`) — verified bit-exact

```
sign(d): x = d+1 (u32); x ^= x>>16; x *= 0x7feb352d; x ^= x>>15; x *= 0x846ca68b; x ^= x>>16;  return (x&1) ? -1 : +1
forward: b[d] = x[d] * sign(d)
         for h in (1,2,4,...,128):  new[t] = (t & h) ? b[t^h] - b[t] : b[t] + b[t^h]
         y[d] = b[d] * 0.0625f
inverse: b = x; same butterflies; y[d] = (b[d] * 0.0625f) * sign(d)
```

Orthonormal (`H256/16` times a sign diagonal), applied identically to q, k and v, so `q.k` is unchanged up to rounding
and the weighted sum of V is un-rotated once at the end (sec. 4.6).

### 3.2 INT8 row + f32 scale (`kvx_store`, `src/kvx_codec.cuh:50-70`) — verified bit-exact

```
sc   = max_d |x[d]| / 127.0f
q[d] = clamp( rn( x[d] / sc ), -127, 127 )        (0 if sc == 0)
```

Row index: `row = kvh*max_ctx + slot` in a layer container, or `row = kvh*stg_rows + tok` in the staging container
(`q35_kvrow`, `q35_kernels.cu:39`). Value = `(float)q[d] * sc`.

Containers (`engine35.cpp:242-253`, `:697-698`):

| Stream of a layer | attn_post writes to | Persistent container |
|---|---|---|
| packed (K of all 16; V of 13) | **staging** `stg_k8_/stg_ks_`, `stg_v8_/stg_vs_`: `[4][stg_rows][256]` int8, `[4][stg_rows]` f32 | `kp`/`vp`: `[4][max_ctx][160]` bytes; `kps`/`vps`: `[4][max_ctx]` f32. `k8/ks` (`v8/vs`) pointers are null |
| unpacked (V of ordinals 10, 14, 15) | `v8/vs` directly at `slot` | `v8`: `[4][max_ctx][256]` int8, `vs`: `[4][max_ctx]` f32 |

Staging is one scratch area shared by all layers, indexed by the row number inside the pass, overwritten by every
layer. `stg_rows = max(Pmax, LAYER_ROWS, 512) = 4096`. It exists only so that a GPU kernel has somewhere to put the INT8
row before the pack kernel reads it. A port can keep the INT8 row in registers/local memory and needs no staging array.

### 3.3 Trellis ("TB") encode — verified bit-exact

Runs right after attn_post for all M rows of the pass, for K and V of all 4 KV heads (`launch_tb_requant`,
`q35_kernels.cu:1079-1087` -> `launch_tb_fast`, `src/kvq_fast.cuh:240-249`): `tbf2_vit_kernel` (`kvq_vit2.cuh`) finds
the paths, `tbf_sel_kernel` (`kvq_fast.cuh:180-235`) selects, packs, or re-stores. `tb_fakequant`
(`kvq_gpu.cuh:54-148`) is the slow single-kernel reference with the same results.

Group of a row: `g = (L*4 + kvh)*2 + kind`, kind 0 = K, 1 = V (`kvq_fast.cuh:72`). Group parameters come from the
profile (sec. 3.6): `mode, kk, tix, guard, m, has_mu, mu[256], U[m][256]`, codebook `ci[256]` (int16) and `gain` of
table `tix`, and the global multipliers `mult[nm]`.

Input `y[d] = (float)q8[d] * sc` (`kvq_fast.cuh:78`).

```
if pos < guard:  x' = y                                   # no trellis; only possible on an unpacked stream
else:
  r = y
  if has_mu:  r[d] = r[d] - mu[d]
  for j in 0..m-1:   cj[j] = f16r( S256(r * U[j]) );  r[d] = r[d] - cj[j]*U[j][d]
  rms = sqrt( S256(r*r) / 256.0f );  if rms == 0: rms = 1
  cdec[s] = ((float)ci[s] * 0.00390625f) * gain            # s = 0..255

  for mi in 0..nm-1:                                       # nm = 9
    s_mi = f16r( rms * mult[mi] );  if s_mi == 0: s_mi = 1
    xi[d] = (int) clamp( rint( (r[d] / s_mi) * 256.0f ), -2048, 2048 )
    for c in 0..3:  st[64c .. 64c+63] = viterbi(xi[64c .. 64c+63])      # 4 independent chains of 64
    rec[d] = cdec[st[d]] * s_mi
    e_mi   = S256( (r - rec)^2 )
  best = the first mi with the smallest e_mi (strict '<')            # kvq_fast.cuh:199-205
  s = s_best, st = st_best
  field[d] = st[d] & 31                                    # low kk bits of the state
  x'[d] = cdec[st[d]]*s  (+ cj[0]*U[0][d]) (+ cj[1]*U[1][d]) (+ mu[d])   # in this order
```

Viterbi (integer only; `kvq_gpu.cuh:104-131`, `kvq_fast.cuh:99-136`; `kk = 5`, state = 8 bits):

```
cost[s] = (s == 0) ? 0 : 0x40000000
for t in 0..63:
  for u in 0..7:   jb[u] = the smallest j in 0..31 minimising cost[j*8 + u];  mn[u] = that cost;  bp[t][u] = jb[u]
  for s in 0..255: cost'[s] = (xi[t] - ci[s])^2 + mn[s >> 5]
  cost = cost'
s = the smallest state with minimal cost
for t in 63..0:  st[t] = s;  s = (bp[t][s >> 5] << 3) | (s >> 5)
```

Transition rule: `state_t = ((state_{t-1} << 5) | field_t) & 255`, start state 0. So the state is the last 8 bits of the
field stream of the chain, and each chain starts fresh. Use 32-bit or wider integers; the largest term is
`(2048+1100)^2 * 64 < 2^31`.

`tbf2_vit_kernel` uses a different but equivalent recursion (keys `32*cost + j`, costs relative to group 0,
"infinity" `32<<24`); it yields the same states. Implement the plain form above.

Every row is encoded independently of all other rows; M does not enter.

### 3.4 Packed row layout (`kvq_fast.cuh:220-231`) — verified

* 256 fields x 5 bits = 1280 bits = **40 little-endian u32 words = 160 bytes** per row (`pks = 32*kb`).
* Bit stream is MSB-first across words: global bit `g = 5*e + jb` (`jb = 0` is the field's MSB) is bit `31 - (g & 31)`
  of word `g >> 5`. Each chain of 64 values is exactly 10 words.
* Scale: `kps/vps[kvh*max_ctx + slot] = s` (f32; it is always a value representable in half precision).
* Nothing else is stored for a packed row (`cj` is not stored; packed streams have `m == 0`).

### 3.5 Decode of a packed row inside attention

State of element `d` (reference `tba_unpack_kernel`, `kvq_attn.cuh:57-58`; kernel form `pk_state_g`,
`q35_atxpk.cu:69-74`):

```
endb = 5*(d+1);  first = 5*(d & ~63);  lo = max(endb - 8, first)
state[d] = the bits [lo, endb) of the MSB-first stream, as an unsigned number (5 bits for the first value of a chain, else 8)
```

Value table used by attention = "decode record" built on the host at load (`kvqaxpk_build`, `kvq_atxpk.hpp:25-58`):

```
tabK[s] = bf16( ((float)ci[s] * 0.00390625f) * gain )      # rounded to BF16, nearest-even
K value used in scores:  tabK[state[d]]          (times kps in the score formula; K mean is NOT added back)
V value:                 tabV[state[d]] * vps + muV[d]     (muV = the group's mean as f32, zeros if !has_mu)
```

**Internal approximations of the reference engine, all intended:**

1. The encoder measures error with the f32 codebook `cdec`; the attention reads the BF16-rounded codebook. The stored
   bits are defined by the encoder, the value by the decoder table.
2. K groups with `has_mu` (ordinals 12 and 13, head 1): the encoder subtracts `mu`, the attention never adds it back.
   A constant added to every key of a head shifts all scores of a query equally and cancels in the softmax. (Profile
   metadata: `"k_groups_skip_mean_addback": true`.)
3. Packed rows are never expanded to INT8 or floats in memory; "no 8-bit copy is made".

### 3.6 Unpacked V streams (ordinals 10, 14, 15)

They are **not** plain INT8 of the projection output. The same encode runs, and `tbf_sel_kernel` writes back
`store8(x')` over the row (`kvq_fast.cuh:234` `else if (!R.cap) kvx_store(x, R.q8, R.qs, R.row, d)`), where `x'` is the
float reconstruction of sec. 3.3 (f32 `cdec`, plus low-rank terms and mean). A row at `pos < guard` gets
`store8(y)` (re-quantising an INT8 row: the scale can move by one ulp). These streams stay INT8 because the pack format
cannot hold `guard != 0` or `m != 0` (`kvqaxpk_packable`, `kvq_atxpk.hpp:60-71`): ordinal 10 has `guard = 1` on V head
1; ordinals 14 and 15 have low-rank terms (`m` = 1 or 2).

### 3.7 `kvq_u5.bin` ("KVQP" profile, 17432 bytes)

Loaded from `YSKN_TB_PROFILE` (`engine35.cpp:27-56`), uploaded verbatim to the device. All fields little-endian.

```
0   char[4]  "KVQP"
4   u16      version = 1
6   u16      header size = 96
8   u32      file size = 17432
12  u32      2            (UNCERTAIN: not read by the engine)
16  u16      D = 256
18  u16      layers = 16
20  u16      kv heads = 4
22  u16      number of codebook tables nt = 2
24  u16      nm = 9       (scale multipliers)
26  u16      1            (UNCERTAIN: not read)
28  u32      ngrp = 128
32  u32      off_m  = 96   -> f32 mult[nm] = 0.90 0.93 0.96 0.98 1.00 1.02 1.04 1.07 1.10
36  u32      off_t  = 132  -> u32 table offsets [nt] = 140, 664
40  u32      off_g  = 1188 -> u32 group offsets [ngrp]
44  u32      17060  offset of a JSON metadata tail;  48  u32  367  its length   (not read by the engine)
52..95       zero

table (524 bytes) at each table offset:
  +0 u8 L = 8 (state bits)   +1 u8 5 (k)   +2 u8 64 (chain length)   +3 u8 8 (fraction bits: value = ci/256)
  +4 f32 gain                 +8 u32 (1 for table 0, 0 for table 1; not read)
  +12 i16 ci[256]             codebook entry of each 8-bit state, |ci| <= 886

group record at each group offset (8 bytes + optional arrays):
  +0 u8 mode      0 = trellis ("CDQ"), 1 = flat 4-bit (not present in this file)
  +1 u8 kk        bits per value (5 everywhere)
  +2 u8 tix       codebook table index
  +3 u8 guard     rows with pos < guard are left unquantised
  +4 u8 m         number of low-rank directions (0..2)
  +5 u8 has_mu    +6,+7 zero
  +8  f16 mu[256]              if has_mu
  then f16 U[m][256]
```

Content of this file: all K groups use table 0 (`gain = 1.0015933513641357`), all V groups table 1 (`gain = 1.0`); the
two `ci` arrays are identical. `mode = 0`, `kk = 5` for all 128 groups. `guard = 1` only for group (ordinal 10, head 1,
V). `m > 0` only for V of ordinal 14 head 1 (`m=1`) and ordinal 15 heads 0..3 (`m = 2,1,1,2`). `has_mu`: 2 K groups
(ordinal 12 head 1, ordinal 13 head 1) and 19 V groups.

Uses: (a) the encoder reads it directly on the device (sec. 3.3); (b) `kvqaxpk_packable` decides per (layer ordinal,
kind) whether the stream is packed: all 4 heads must have `mode == 0, kk == kb, guard == 0, m == 0` -> K 16/16, V 13/16;
(c) `kvqaxpk_build` makes the decode records: per group 2048 bytes = `[0,512)` 256 x BF16 codebook, `[512,1536)`
256 x f32 mean (zeros without `has_mu`), `[1536,1792)` INT8 codebook and `[1792,1796)` its f32 step (QK8 mode only, unused),
followed after all records by one flag byte per group.

---------------------------------------------------------------------------------------------------------------------

## 4. The attention computation

Same in pk3 (`q35_atxpk3.cuh:131-477`) and pk4 (`q35_atxpk4.cuh:62-419`); row bookkeeping in `pk_row`
(`q35_atxpk.cu:390-410`); merge in `q35_combine_kernel` (`q35_kernels.cu:1279-1363`).

### 4.1 Row geometry, GQA

```
base = *d_pos                                  # first slot of this launch
dp   = wk ? wk[tk] : tk                        # depth of row tk
len  = base + dp + 1                           # number of visible keys
own  = base + tk                               # slot holding this row's own K and V
keys of row tk:  slots 0 .. len-2   ("prefix")   and   slot `own`   (in the role of key index len-1)
query head hd (0..23) uses KV head  kvh = hd / 6          (hd = kvh*6 + i, q35_atxpk.cu:395)
```

Causality is the range only; there is no mask. The row's own key and value are read from the cache, i.e. after
quantisation (sec. 3).

### 4.2 Scores

```
dot(j)   = sum_d q[d] * Kval_j[d]              q = bf2f(q16), Kval = tabK[state] (BF16 values)
score(j) = dot(j) * ( ks_j * 0.0625f )         ks_j = kps[kvh*max_ctx + j];  0.0625 = 1/sqrt(256)
```

(`q35_atxpk4.cuh:325`, `:386`.) Prefix dots are tensor-core `mma.m16n8k16` BF16xBF16 -> f32 accumulations
(`pk_mma`, `q35_atxpk.cu:35-38`): each product is exact in f32, the order and rounding of the 16-term accumulation
inside the instruction are not specified. The own-key dot is ordinary f32 with a fixed order
(`q35_atxpk4.cuh:367-383`):

```
for r in 0..3:  p[r] = 0;  for kq in 0..15:  for e in (16kq+2r, 16kq+2r+1, 16kq+8+2r, 16kq+9+2r):  p[r] = p[r] + q[e]*K[e]
dot_own = (p[0] + p[1]) + (p[2] + p[3])
```

### 4.3 Splits

```
S   = (len < 4096) ? 31 : 42
per = (len + S - 1) / S
split s:  lo = s*per;  hi = min(len, lo + per);  ph = max(lo, min(hi, len-1))     # prefix keys of this split = [lo, ph)
          has_self = (lo <= len-1 < lo + per)
```

### 4.4 Partial softmax of one (row, head, split)

State `m = -1e30f, l = 0, o[0..255] = 0`. `LOG2E = 1.4426950408889634f`.

Prefix keys are processed in **tiles of 32 consecutive slots aligned to absolute multiples of 32** (`PK_KT = 32`,
`tbeg = (klo/32)*32`); a tile contributes only its keys inside `[lo, ph)` (others get score `-inf`). A tile with no key
of this row changes nothing. For each tile with at least one key (`q35_atxpk4.cuh:249-351`):

```
x  = max(m, max_j score(j))
c  = exp2f( (m - x) * LOG2E );   m = x;   y0 = x * LOG2E
p_j = exp2f( score(j) * LOG2E - y0 )                         # masked keys: 0
l  = l * c + sum_j p_j
o[d] = o[d] * c                                              # skipped when c == 1 for the whole warp; same value
o[d] = o[d] + sum_j bf16( p_j * vs_j ) * Vraw_j[d]           # tensor-core MMA; note the BF16 rounding of p_j*vs_j
```

`Vraw_j[d]` = `tabV[state]` (packed, BF16) or `(float)v8` (unpacked; exact in BF16). `vs_j` = `vps`/`vs` of slot j.
Order of `sum_j p_j`: `t[r] = sum over jj=0..3 of ( p[8jj+2r] + p[8jj+2r+1] )` for `r = 0..3` (key index inside the
tile), then `(t[0]+t[1]) + (t[2]+t[3])`.

Then the own key, if `has_self` (`q35_atxpk4.cuh:385-402`), in plain f32 and **without** the BF16 rounding:

```
sc = dot_own * ( ks_own * 0.0625f )
mn = max(m, sc);  c = exp2f((m - mn)*LOG2E);  p = exp2f((sc - mn)*LOG2E);  m = mn;  l = l*c + p
pv = p * vs_own
o[d] = o[d]*c + pv * Vraw_own[d]
```

Finally, for a packed V stream only (`q35_atxpk4.cuh:405-412`): `o[d] = o[d] + l * muV[d]`.

Outputs: `pm[(tk*42 + s)*24 + hd] = m`, `pl[...] = l`, `pacc[((tk*42 + s)*24 + hd)*256 + d] = o[d]`. Splits `s >= S`
are not written and not read.

### 4.5 Combine (`q35_kernels.cu:1285-1353`)

Per (row, head):

```
M2  = max( -1e30, max_{s<S} pm[s] )
w_s = (pm[s] > -1e29f) ? expf(pm[s] - M2) : 0
L   = 0;  for s = 0..S-1 in order:  L = L + w_s * pl[s]
O[d]= 0;  for s = 0..S-1 in order:  O[d] = O[d] + w_s * pacc[s][d]
y[d]= O[d] / L                                  # still in the rotated domain; no rbf here when KVX_ROT
```

(The S<=32 and S<=64 branches differ in layout only; unused slots contribute `+ 0*0`.)

### 4.6 Un-rotate, gate, INT8 for o_proj (`q35_kernels.cu:1354-1362`)

```
y  = rbf( rotate_inverse(y) )                                   # per head, sec. 3.1
sg = rbf( 1.0f / (1.0f + expf(-bf2f(gate16[tok][head][d]))) )   # q35_sigmoid :115
y2 = rbf( y * sg )
b[e] = y2 * (float)o_proj.signs_in[e]                           # e = head*256 + d, 6144 values per row
per 512-block (2 heads) (a8_group512_tail, a8_prologue.cuh:122-133):
  v     = f16r( FWHT512(b)[i] * 0.044194173824159216f )
  alpha = max_i |v[i]| / 127.0f                                 # one alpha per 512-block: alpha[tok*12 + blk]
  qx[i] = clamp( rn( v[i] / alpha ), -127, 127 )   (0 if alpha == 0)
```

`qx` layout: `qx[tok*6144 + blk*512 + i]`, or the interleaved "lo" layout `qx[blk*4096 + a8_lo_off(i, tok)]` when
`lo_now_` (M <= 8; `a8_prologue.cuh:121`). The layout is the GEMV's input convention (common part).

---------------------------------------------------------------------------------------------------------------------

## 5. Positions, multi-row passes, commit

### 5.1 `d_pos`

A device int holding `pos_`, the number of committed tokens = slot of row 0 of the next pass. Kernels read it at run
time (so one captured graph serves every position). For a sliced prefill launch the slice gets its own value
`base + s` (`launch_q35_pos_slices`, `engine35.cpp:723`).

### 5.2 Rows of the same pass

Order inside a layer: all rows' K/V are written and packed first, then attention runs. Row `tk` reads slots
`0 .. base+tk-1` and its own slot, which include rows `0..tk-1` of the same pass. No special case: earlier rows of the
pass are ordinary cache rows by the time attention runs.

### 5.3 Tree passes (`wk`)

With a table, rows `0..L` are the chain (depth = row index) and rows `L+1..` are leaves: alternative tokens for depth
`wk[tok] <= L` (`q35_bdraft.cu:699-704`). A leaf at depth `dp` stored in slot `base+tok`:

* RoPE uses position `base+dp` (sec. 2.5);
* it sees keys `0 .. base+dp-1` (cache + chain rows `0..dp-1`) and its own slot (sec. 4.1); it never sees chain rows of
  depth >= dp or other leaves, and nothing sees it.

If the leaf is accepted, `lfrow_promote(src=leaf row, dst=depth)` copies its K/V from slot `base+src` to `base+dst`
in every layer: packed payload row (160 bytes) and scale, or INT8 row and scale (`engine35.cpp:877-900`,
`wk_promote2_kernel` `lfrow_kernels.cuh:333-355`). Byte copy, no re-encode.

### 5.4 Commit

`Engine35::commit(n_accept)` (`engine35.cpp:1319-1330`): `pos_ += n_accept + 1`, `d_pos` updated. For the attention
layers **nothing else happens**: the K/V of accepted rows are already at their final slots with their final bits
(encode is per-row), and rows of rejected tokens sit at slots `>= pos_`, which no later row reads before the next pass
overwrites them. `replay_commit` only concerns the linear-attention layers' recurrent state. Rewinding to a snapshot is
likewise just setting `pos_` (`engine35.cpp:1581`).

---------------------------------------------------------------------------------------------------------------------

## 6. Memory

Per token:

| Item | Bytes |
|---|---|
| packed stream of one layer: 4 heads x (160 + 4) | 656 |
| INT8 stream of one layer: 4 heads x (256 + 4) | 1040 |
| K, 16 layers packed | 10,496 |
| V, 13 packed + 3 INT8 | 8,528 + 3,120 |
| **Total per token** | **22,144 (21.6 KiB)** |
| (all-INT8 reference: 16 x 2 x 1040) | 33,280 |

| Context | KV total |
|---|---|
| 8,192 | 181,403,648 B = 173.0 MiB (matches the log line; INT8 reference 260.0 MiB) |
| 32,768 | 692 MiB |
| 131,072 | 2.70 GiB |
| 262,144 | 5.41 GiB |

Fixed scratch (CUDA engine; a port may differ): staging `2 x 4 x 4096 x 260` = 8.5 MB; `pm`, `pl`
`64 x 42 x 24 x 4` = 258 KB each; `pacc` `64 x 42 x 24 x 256 x 4` = 66 MB; `q16`, `gate16` `Pmax(1024) x 6144 x 2`
= 12.6 MB each; RoPE table `max_ctx x 32 x 8` B (2.1 MB at 8192); decode records 262,272 B; profile 17,432 B.
`max_ctx` is rounded up to a multiple of 64 (`engine35.cpp:219`).

---------------------------------------------------------------------------------------------------------------------

## 7. Simplest correct implementation

### 7.1 Must match bit-for-bit (integer or fixed-order float; all achievable)

1. Sec. 2.3-2.6 given the common-part `y`: norm sum order (`S256`), `1 + w`, every `rbf`, RoPE pairing and table.
2. Rotation (sec. 3.1) and `store8` (3.2).
3. The trellis encode (3.3): `S256` order for the low-rank dots, `rms` and the 9 errors; `f16r` of the scale; `rint`
   and clamp of `xi`; Viterbi tie rules (smallest `j`, smallest final state); first-minimum over `mi`; the bit packing
   (3.4); for unpacked V the re-`store8` of `x'` (3.6), including the `guard` row.
4. Decode values: BF16-rounded codebook, `kps*0.0625`, V mean added as `l*mu`, K mean not added (3.5).

If these match, the cache contents are identical to the CUDA engine's (compare with `yskn_state_hash`, sec. 8), and all
remaining differences are float rounding inside sec. 4.

### 7.2 Cannot match bit-for-bit; replicate the structure to stay close

* Tensor-core dot products (order inside `mma` unspecified), `exp2f`/`expf` last bits.
* The **BF16 rounding of `p_j * vs_j`** for prefix keys (sec. 4.4) is a real, systematic part of the reference
  result (8 significant bits, relative error up to 2^-9 per key weight). Round to BF16 there too if you want logits within float noise of CUDA;
  skipping it gives a slightly more accurate, different result. The own key is not rounded.
* Online-softmax grouping: tiles of 32 slots on absolute boundaries, splits of sec. 4.3, combine order of sec. 4.5.
  A port that uses one pass over all keys with a single running max gets the same value up to ~1e-6 relative; keeping
  the tile/split structure keeps the BF16 rounding points identical (the rounding of `p_j*vs_j` depends on the running
  max at that tile).

Simplest form that keeps everything that matters:

```
for row tk, head hd:  kvh = hd/6
  for s in 0..S-1:  (m,l,o) = partial(keys [lo,ph) by 32-aligned tiles, then own key if has_self)   # sec. 4.4
  combine                                                                                          # sec. 4.5
un-rotate, gate, FWHT512, INT8                                                                     # sec. 4.6
```

Decode states on the fly: per key row, read 40 words, produce 256 table indices (first value of each chain 5 bits,
then a sliding 8-bit window stepping 5 bits).

### 7.3 Performance techniques only (do not affect values beyond 7.2)

Staging container; `early`/PDL launch overlap and the volatile re-read of `d_pos`; CTA shapes `<1,4>/<3,4>/<6,2>`,
`TPC`, `SKEW`, `NST` stage rings, `cp.async`; the pair LUT (`PLUT`, 8192 entries indexed by a 13-bit window = two
adjacent states); `ldmatrix` layouts and the `^ (key & 7)` swizzle; `tbf2_vit_kernel`'s packed-key recursion; the
`TBF_TILE = 128` batching and scratch buffer of the encoder; the `S <= 32 / <= 64` branches of combine; the `lo` qx
layout (it is a layout the GEMV expects, not a numeric change); cuda graphs.

### 7.4 Suggested order

Milestone A (1-row decode): sec. 2 -> 3.1 -> 3.2 -> 3.3/3.4 -> attention with `S = 1` semantics checked against the
split form. A useful intermediate target is `YSKN_TB_CAP=0` on the CUDA side: then all streams are INT8 containers
holding `store8(x')` (the encode of sec. 3.3 still runs, K mean added back), `yskn_kv8_read` works, and the attention
is the INT8 form. With `YSKN_TB_CAP=1` `yskn_kv8_read` throws for every layer (`engine35.cpp:525`, `k8` is null).

---------------------------------------------------------------------------------------------------------------------

## 8. Oracles in `build/libyskn.so` (all present in the Linux build)

| Export | Use |
|---|---|
| `yskn_kvx_probe(x, q, scales, y, rows)` (`kvx_probe.cu:22`) | rotation + `store8` + inverse; rows even |
| `yskn_tb_probe(blob, len, x, grp, pos, y, rows)` (`:44`) | slow reference of sec. 3.3: `y = x'` for input `x` (no INT8 step) |
| `yskn_tbp_pack_unpack(blob, len, x, grp, pos, rows, stride=160, y_fq, pay, side, y_dec, ms)` (`:123`) | fast encoder: payload words, `side[2r] = scale bits`, `side[2r+1] = f16(cj0) | f16(cj1)<<16`, and the f32-codebook decode |
| `yskn_atxpk_bench(...)` (`:322`) | packed attention on synthetic keys: `o_pk[M][24][256]` = sec. 4.1-4.5 output (rotated domain, no gate) |
| `yskn_state_hash` (`engine35.cpp:1585-1609`) | FNV hashes of `kp, vp/v8, kps, vps/vs` rows `[0,pos)` per attention layer, in the live engine |
| `yskn_set_capture / yskn_capture_rows` | hidden states after chosen layers |

A NumPy reference of sec. 3 that reproduces the first three bit-for-bit is in the scratch file
`ref.py` (temporary location; copy it
if you want to keep it).

---------------------------------------------------------------------------------------------------------------------

## 9. UNCERTAIN / not determined

1. Bit-equality of the common-part output `y` between xsm mode 0 (M <= 16) and mode 3 (M > 16), i.e. whether a
   prefilled row and the same row written by a decode pass have identical cache bits. Not checked; outside this area.
2. `cosf/sinf/rsqrtf/expf/exp2f` last-bit behaviour of the CUDA maths library versus the Intel one (sec. 2.5, 4.4).
3. Internal accumulation order and rounding of `mma.sync` (sec. 4.2); attention outputs cannot be compared for
   equality, only within a tolerance. I did not measure the tolerance.
4. Sections 2, 4 and 5 were not executed against the engine (no full-model run; the GPU was occupied by another
   process holding 14.5 GB). The attention formulas are from the pk3/pk4 sources; the `q35_atxpk_kernel` v1 and
   `q35_atx.cu` variants show the same formulas.
5. Profile header fields at offsets 12 and 26 and the table word at +8 are not read by the engine; meaning unknown.
6. The HCAP leaf selection that fills `wk` (drafter side) is described only as far as attention needs it.

---------------------------------------------------------------------------------------------------------------------

## Errata (from the executable reference `ref/core.py` class `Attn`, checked against CUDA hidden-state captures)

No formula in sections 2-5 had to be changed. Sections 2, 4 and 4.6 have now been executed (CPU reference vs. captures of a
12-row prefill pass, `ref/run.py`, `ref/run_tf.py`): with the layer input taken from the capture, rows come out bit-identical
after layers 3, 7 (packed K+V) and 63 (INT8 V with low-rank terms and mean). Corrections / additions:

1. Sec. 8, last paragraph: the NumPy codec reference now lives in `xe/ref/kv_codec_ref.py` (the scratch path is stale).
2. Sec. 9 item 3 / sec. 7.2 (tolerance, measured). The largest source of CUDA-vs-port differences in an attention layer is
   **not** the tensor-core order but the last bit of the per-row `r_tok = rsqrtf(...)` of sec. 2.2 (common part): one ulp of
   `r_tok` flips the BF16 rounding of a few of the 14336 `y` values of the row. Flips in q/k mostly vanish in the RMS norm
   (sec. 2.4 is scale invariant); a flipped **V** element changes the cache row and therefore that row and later rows that
   attend to it (hidden-state relative error ~1e-3 per affected row after the layer). Next in size: accumulation order of
   the prefix-key dot products (relative score noise ~1e-7 flips 1-3 BF16 values of the 6144 outputs per row; most are
   absorbed by the INT8 step of sec. 4.6). `exp2f`/`expf` last bits: no visible effect.
3. Sec. 4.4, consequence worth stating: when `len < 31` every split holds one key, so `p_j = 1` exactly and the weight of a
   prefix key is `bf16(vs_j)`: prefix values use the BF16-rounded scale, the own key the f32 scale. Dropping this
   rounding costs the bit-identical rows (verified at layer 7).
4. Sec. 4.4: a tile (or split) without keys of the row leaves `(m, l, o)` unchanged also when `m` is still `-1e30`
   (`c = exp2f(0) = 1`, all `p_j = 0`); splits `s >= len` are written as `(-1e30, 0, 0)` and get weight 0 in combine.
5. Not exercised by the captures (still source-reading only): `wk` depth tables (sec. 5.3), `len >= 4096` (S = 42),
   slices of 64 rows / the pk3 kernel, tiles holding more than one key of a row (needs a context > 31 tokens), the
   `guard` row of ordinal 10 at position 0 when it is not the only key.
