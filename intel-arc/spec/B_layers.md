# Spec B — per-layer math of Engine35 (everything except the compressed GEMM and full attention)

Source tree: `..` (commit c0e11b9). All `file:line` references are relative to `src/`.
Model facts were read from the header of `models/speed/qwen38_27b_speed.yskn`.
Items I could not settle from the source are marked **UNCERTAIN**.

Contents
0. Defaults: which path to implement, which variants to ignore
1. Constants, buffers, layouts for M rows
2. Scalar helpers: bf16 / fp16 rounding, reductions, FWHT512, pm math
3. The int8 input quantisation ("a8 tail") that ends most kernels
4. The XSUM contract with the GEMM (acc / us / S / r)
5. Residual stream: embed, `res_norm_xsm`, where ln1 / ln2 / final norm are applied
6. Gated DeltaNet layer: ab, mix, conv, state, gdn_out, n_commit, replay
7. MLP (`q35_mlp`)
8. Final norm, head handoff, argmax / topk / nll
9. Capture, misc kernels, files that are only named
10. Non-default variants (short)
11. Uncertainties and porting hazards

---------------------------------------------------------------------------------------------------

## 0. Defaults

### 0.1 Decided from the constructor, `engine35.hpp`, `c_api.cpp` and the Python package

| switch | default | where | effect |
|---|---|---|---|
| `xsm_` (XSUM, exact int64 sums) | **1 (on)** | `engine35.hpp:248` `int xsm_ = 1;`, env `YSKN_Q35_XSUM` (`engine35.cpp:211`) | `pass_body` sets `xsmc_ = xsm_ != 0` (`engine35.cpp:825`) → `res_norm()` always takes the `launch_q35_res_norm_xsm` branch (`engine35.cpp:562-566`) |
| `xsm_part_` | 0 | `engine35.hpp:254`, env `YSKN_Q35_XSM_PART` | `nmode(M)` = 0 for M ≤ 16 (`engine35.hpp:255`) |
| `opt_flow_` (flow_mlp) | **0 (off)** | `engine35.hpp:292`, env `YSKN_Q35_FLOW`, set_opt key 14 | `flow_ok()` false → `mlp_block` is used (`engine35.cpp:795-796`) |
| `opt_fin_` ("fin") | **0 (off)** | `engine35.hpp:297`, env `YSKN_Q35_FIN`, set_opt key 7 | `gemv_res` never takes the fused branch (`engine35.cpp:667`) |
| `opt_lo_` ("lo") | **1 (on)** | `engine35.hpp:307`, env `YSKN_Q35_LO`, set_opt key 28 | for M ≤ 8 the int8 input `qx` is written in an interleaved layout (§3.2). Layout only; values identical |
| `opt_kconv_` | 1 | `engine35.hpp:366`, set_opt key 5 | verify passes with M ≤ 16 keep q/k/v/beta/g per layer for the replay (§6.7) |
| `kz_` (PMATH) / pm mode | **0 (off)** | `engine35.hpp:289`, env `YSKN_Q35_PMATH`, set_opt key 21; `apply_kz()` `engine35.cpp:458-465` | `q35_pm_on = 0` → CUDA `rsqrtf/expf/log1pf` are used, NOT `pm_math.cuh` (§2.5) |
| `l2pf_`, `l2pf2_mb_` | 0 | `engine35.hpp:301,304` | L2 prefetch hints only; ignore (`Q35Pf`, `q35_l2pf`) |
| `opt_lfrow_` (tree rows, `wk`) | 0, but see §10.4 | `engine35.hpp:362`, set_opt key 6 | `launch_wk_conv/_state` instead of `launch_q35_conv/_state` |
| `pfa_`, `Pmax_`, `PM_` | 1, 1024, 1024 | `engine35.cpp:193-197`, Python env `YSKN_Q35_PMAX=1024` | prefill passes of up to 1024 rows (§10.3) |
| `Mmax_`, `HM_` | 64, 16 | `engine35.hpp:238,313` | scratch sizes; the head runs in slices of ≤ 16 rows |
| `eps_` | 1e-6 | model header `eps` (`engine35.cpp:218`) | RMS-norm epsilon |

The Python package sets only these environment variables (`python/yasakani/runtime.py:27-42`) and **removes every other `YSKN_*`**
variable before opening the model (`runtime.py:93-100`): `YSKN_BDR_SHEAD_M=512, YSKN_Q35_G6=1, YSKN_Q35_KV8=1, YSKN_Q35_KV_FP16=0,
YSKN_Q35_LAYER_OVERLAP=1, YSKN_Q35_LAYER_ROWS=4096, YSKN_Q35_PFALT=1, YSKN_Q35_PMAX=1024, YSKN_Q35_SHARED_WEIGHTS=1, YSKN_Q35_ATX=1,
YSKN_TB_CAP=1, YSKN_TB_VIT2=1, YSKN_ATX_PK3_PLUT=1, YSKN_ATX_PKV=3, YSKN_ATX_VSP=7`. None of them touches XSUM / FLOW / FIN / LO / PMATH.
`engine.py` calls `yskn_set_opt` only with the sampling keys 10, 11, 12, 13, 16, 29 (`engine.py:20, 166-177`).
`c_api.cpp:92` forwards `yskn_set_opt` unchanged. So the table above is the shipping configuration.

### 0.2 The default chain for one pass of M ≤ 16 rows (`pass_body`, `engine35.cpp:824-842`)

```
res_norm(nullptr, ln1[0])            -> q35_embed + q35_res_norm_xsm (embedding branch)        §5
for l in 0..63:
  if GDN layer:
     gemv(qkv, z)                    [GEMM, not this spec]
     q35_mix (with the a/b rows fused, M <= 32)                                                §6.1-6.2
     q35_conv                                                                                 §6.3-6.4
     q35_state                                                                                §6.5
     q35_gdn_out                     -> qx/alpha for out_proj                                  §6.6
     gemv(out); q35_res_norm_xsm(part=out, w=ln2, sg_in=gate.signs_in)                         §5
  else: attn_block ... gemv(o); q35_res_norm_xsm(part=o, w=ln2, sg_in=gate.signs_in)
  mlp_block: gemv(gate, up); q35_mlp -> qx/alpha for down_proj                                 §7
  gemv(down); q35_res_norm_xsm(part=down, w = next ln1 or final norm, sg_in = next qkv/q or head) §5
  (optional) copy h to the capture slot                                                        §9
head_slice: GEMM(head); argmax (+ topk8 / nll / sampler)                                      §8
```

### 0.3 Variants you can ignore for a first port

* `launch_q35_res_norm`, `launch_q35_res_norm_row`, `launch_q35_residual` (struct `Q35ResNormArgs`): the **non-XSUM** path
  (`YSKN_Q35_XSUM=0`), float partial sums. Math in §10.1 for reference only.
* `flow_mlp` / `q35_flow.cu` (FLOW), `opt_fin_` (FIN), `xsm_part_` (mode 2), `q35_xsm_xn` (only when the old MTP drafter is loaded,
  `engine35.cpp:838`), `q35_chk.cu`, L2 prefetch, `pdl_wait/pdl_trigger`, CUDA-graph plumbing, the `cnt` spin barriers.
* The `*_pf_kernel` twins (`q35_mlp_pf_kernel`, `q35_mix_pf_kernel`, `q35_res_norm_pf_kernel`, `q35_gdn_out_pf_kernel`,
  `q35_ab_pf_kernel`, `q35_state_pf_kernel`, `q35_conv_pf_kernel`, `q35_state_nb_kernel`): same math, laid out for M > 64 prefill
  (`q35_pf_launch(M) = M > 64 && YSKN_Q35_PFALT`, `q35_kernels.cu:602`). See §10.3.
* "lo" is on by default but is only a storage layout of `qx` agreed between these kernels and the GEMM. If your GEMM reads the
  plain layout, write the plain layout; nothing else changes.
* `post_kernels.cu`: belongs to the **other** engine (`engine.cpp`, non-qwen3_5 archs, fp16 residual stream, 3584-wide). Engine35 does not
  call any of it (no `launch_qkv_post / launch_residual_post / launch_xsum_res_xform / launch_norm_xform_a8` in `engine35.cpp`).
  It is the fp16 ancestor of the kernels described here; ignore.

---------------------------------------------------------------------------------------------------

## 1. Constants, buffers, layouts

`q35.hpp:13-20`:
```
Q35_H = 5120 (hidden)          Q35_GK = 16 key heads      Q35_GV = 48 value heads     Q35_GD = 128 (head dim, key and value)
Q35_CONV = 10240 (= 2*16*128 + 48*128, the qkv width)     Q35_Z = 6144 (= 48*128)     Q35_SST = 16 (stride of S_ per row)
F = 17408 (ffn, 34 blocks of 512)   V = 248320   nlayers = 64, full attention at l = 3, 7, ..., 63 (l % 4 == 3)
```

All per-row buffers are row-major with the row (`tok`, 0..M-1) as the outer index. `tok` is the row of the pass, position = `*d_pos + tok`.

| buffer | dtype | shape / index | producer → consumer |
|---|---|---|---|
| `h_` | f32 (values always bf16-representable) | `[M][5120]`, `h_[tok*5120 + n]` | residual stream, `res_norm_xsm` |
| `qx_` | int8 | plain: `[M][N]`, `qx[tok*N + i]`; lo: §3.2 | a8 tail → next GEMM. Sized `Pmax * max(F, Q35_Z)` (`engine35.cpp:324`) |
| `alpha_` | f32 | `[M][N/512]`, `alpha[tok*(N/512) + blk]` | a8 tail → next GEMM |
| `us_` | f32 | `[M]` | GEMM → post kernels (§4) |
| `S_` | f32 | `[M][16]`, 10 used: `S_[tok*16 + blk]` | `res_norm_xsm` → `q35_xsm_r` users |
| `xn_` | bf16 (u16) | `[M][5120]` | `res_norm_xsm` (`xu`) → a/b projection |
| part of a tensor (`t.part`) | **int64** in the default mode (declared `float*`) | `[M][N]`, `acc[tok*N + n]` | GEMM → post kernels; **read-and-clear** (§4) |
| `Y.pmix` (`mixed`) | bf16 | `[M][10240]` | `q35_mix` → `q35_conv`, `q35_state` (hist), replay |
| `z_` | f32 (bf16-valued) | `[M][6144]` | `q35_mix` → `q35_gdn_out` |
| `Y.pab` (`ab`) | f32 (bf16-valued) | `[M][96]`: `[0..47]` = b, `[48..95]` = a | ab → conv |
| q, k | f32 | `[M][16][128]` | conv → state (`Y.tq/Y.tk` if M ≤ 16, else `gq_/gk_`) |
| v | f32 (bf16-valued) | `[M][48][128]` | conv → state |
| beta, g | f32 | `[M][48]` | conv → state |
| `core_` | f32 (bf16-valued) | `[M][48][128]` = `[M][6144]` | state → gdn_out |
| `Y.S` (GDN state) | f32 | `[48][128][128]`, `S[h*16384 + i*128 + j]`, i = key dim, j = value dim | persistent per GDN layer, 3.1 MB each |
| `Y.hist` | bf16 | `[3][10240]`, `hist[k*10240 + c]`, k = 0 oldest … 2 newest | persistent per GDN layer |
| `emb_rows_` | bf16 | `[M][5120]` | `q35_embed` → `res_norm_xsm` |
| `logits_` | f32 | `[rows ≤ 16][V]` | argmax → topk / nll / sampler / `yskn_logits` |

Per-layer vectors, all **bf16, stored exactly as in the checkpoint** (`Engine35::vec`, `engine35.cpp:168-179`, rejects anything but
`dtype == "bf16"`; `tools/convert/yskn_convert.py:69` "stored as they are (bf16)"). Names in the file (`engine35.cpp:256-271`):

| member | file name | length | layout |
|---|---|---|---|
| `ln1` | `layers.L.input_layernorm` | 5120 | |
| `ln2` | `layers.L.post_attention_layernorm` | 5120 | |
| `fnorm_` | `norm` | 5120 | |
| `wa` | `layers.L.linear_attn.in_proj_a` | 48 × 5120 | `wa[r*5120 + i]`, row r = value head |
| `wb` | `layers.L.linear_attn.in_proj_b` | 48 × 5120 | same |
| `conv` | `layers.L.linear_attn.conv1d` | 10240 × 4 | `conv[c*4 + tap]`, tap 3 multiplies the current row |
| `A_log` | `layers.L.linear_attn.A_log` | 48 | |
| `dt_bias` | `layers.L.linear_attn.dt_bias` | 48 | |
| `gnorm` | `layers.L.linear_attn.norm` | 128 | shared by all 48 heads |

fp16 appears in exactly two places of this area: (a) `round_f16` inside the a8 tail (§3), (b) the coded embedding table
(`lut16`, `scale16`, §5.1). Everything else is bf16 or f32.

Per-tensor vectors from the loader (GEMM spec owns them; used here as given):
`tau[n]` f32, one per output row (`loader.cpp:19-32`: fp16 row scale × a LUT gain); `signs_in[Kc]`, `signs_out[N]` int8 ±1
(`loader.cpp:34-36`, `signs.hpp` rule `mulberry32_7919_13`). `lm_head` has `rot_out = None`: no output rotation, no `signs_out`.

---------------------------------------------------------------------------------------------------

## 2. Scalar helpers

The CUDA is compiled with `-O3 --fmad=false` (`CMakeLists.txt:50`) and nearly all arithmetic uses the `__fadd_rn/__fmul_rn/__fdiv_rn`
intrinsics: every operation is a single IEEE-754 binary32 round-to-nearest-even operation, never fused. In SYCL: compile with
`-fp-model=precise -ffp-contract=off` (icpx defaults to fast-math) and make sure IGC does not form `mad`.
Below, `a*b`, `a+b`, `a/b` mean exactly one such operation; parentheses give the order.

### 2.1 bf16 and fp16 (`q35_kernels.cu:35-37`, `a8_prologue.cuh:36-38`)
```
bf2f(u16 u)  = float_from_bits(u << 16)                                  // exact
f2bf(f)      = __float2bfloat16_rn(f): round f32 to the nearest bf16 (ties to even), keep the top 16 bits
               for finite f:  u = bits(f);  u += 0x7fff + ((u >> 16) & 1);  return u >> 16;     (overflow rounds into inf)
rbf(f)       = bf2f(f2bf(f))                                             // "round to bf16, keep as f32"
h2f(u16)     = IEEE half -> float (exact);  f2h(f) = float -> half, round-nearest-even;  round_f16(f) = h2f(f2h(f))
               (|f| >= 65520 becomes inf; subnormal halves are kept)
```
**UNCERTAIN**: NaN payload behaviour of `__float2bfloat16_rn`; irrelevant unless something already went wrong.

### 2.2 Reductions (float sums are order-sensitive; these orders are used everywhere)
* `xor_tree32(a[0..31])` — the warp shuffle loop `for (o = 16; o; o >>= 1) acc += shfl_xor(acc, o)`:
  `b[i] = a[i] + a[i^16]; c[i] = b[i] + b[i^8]; d[i] = c[i] + c[i^4]; e[i] = d[i] + d[i^2]; result = e[0] + e[1]`.
  (All lanes end with the same value because float + is commutative.) Equivalent closed form for lane 0:
  level 1 pairs (i, i+16); level 2 pairs (i, i+8) of those; then +4, +2, +1.
* `seq4(s0,s1,s2,s3) = ((s0 + s1) + s2) + s3`.
* Max reductions (amax) are order-independent.

### 2.3 FWHT over 512 (`a8_prologue.cuh:41-51, 87-98`; `q35_kernels.cu:73-79, 247-259`)
All implementations (shared-memory, warp-shuffle, per-warp register) do the same thing bit for bit:
```
for h in 1, 2, 4, ..., 256 (in this order):
    for every pair (lo, hi = lo + h) with (lo & h) == 0:   (a, b) = (x[lo], x[hi]);  x[lo] = a + b;  x[hi] = a - b
```
unnormalised; the callers then multiply each element by `K512 = 0.044194173824159216f` (= 1/sqrt(512), f32 0x3D3504F3).
"Inverse rotation" of a GEMM output (`q35_inv_h512s`, `q35_kernels.cu:73-78`):
```
y[i] = (fwht512(x)[i] * K512) * sign_out[i]            // sign as float ±1, applied AFTER the transform
```
"Forward rotation" of a GEMM input: `x[i] * sign_in[i]` BEFORE the transform (§3).

### 2.4 Math functions actually used (`q35_kernels.cu:42-44, 113-116`)
```
q35_rsqrt(x)    = pm ? pm_rsqrt(x)    : rsqrtf(x)
q35_exp(x)      = pm ? pm_expf(x)     : expf(x)
q35_silu(x)     = pm ? pm_silu(x)     : x / (1.f + expf(-x))
q35_sigmoid(x)  = pm ? pm_sigmoid(x)  : 1.f / (1.f + expf(-x))
q35_softplus(x) = pm ? pm_softplus(x) : (x > 20.f ? x : log1pf(expf(x)))
```
`pm` is the `__constant__ int q35_pm_on`, set by `q35_set_pm` from `Engine35::apply_kz()`:
```
int on = kz_ ? 1 : 0;  { const char* p = getenv("YSKN_Q35_PM"); if (p && p[0]) on = atoi(p) ? 1 : 0; }      // engine35.cpp:459-460
q35_set_pm(on, st_); q35_flow_set_pm(on, st_); pfa_set_pm(on, st_);                                       // engine35.cpp:462
```
**pm mode is OFF by default** (`kz_ = 0`, `YSKN_Q35_PM` unset, and the Python package strips it). The default therefore uses
CUDA's libm `rsqrtf`, `expf`, `log1pf`, which are not correctly rounded and cannot be reproduced bit-for-bit on another GPU.
See §11 for what that means for validation.

### 2.5 `pm_math.cuh`, exactly (float part; every operation is a single RN op)

`pm_f(bits)` = float from a hex bit pattern.

```
pm_rsqrt(x) = 1.f / sqrt(x)                       // IEEE sqrt then IEEE divide                       pm_math.cuh:16

pm_expf(x):                                                                                          pm_math.cuh:18-38
  ux = bits(x); ax = ux & 0x7fffffff; neg = ux >> 31
  if ax > 0x7f800000: return x + x                                   // NaN
  if !neg && ax >= 0x42b17218: return +inf                           // x >= 88.7228394
  if ux > 0xc2cff1b5: return 0.f                                     // x < -103.972084 (incl. -inf)
  LN2HI = pm_f(0x3f317200); LN2LO = pm_f(0x35bfbe8e)
  hi = 0; lo = 0; r = x; k = 0
  if ax > 0x3eb17218:                                                // |x| > 0.5 ln2
     if ax < 0x3f851592:                                             // |x| < 1.5 ln2
        hi = x - (neg ? -LN2HI : LN2HI); lo = neg ? -LN2LO : LN2LO; k = neg ? -1 : 1
     else:
        k = trunc_to_int( pm_f(0x3fb8aa3b) * x + (neg ? -0.5f : 0.5f) );  t = (float)k
        hi = x - t * LN2HI;  lo = t * LN2LO
     r = hi - lo
  else if ax < 0x39000000: return 1.f + x                            // |x| < 2^-13
  t = r * r
  c = r - t * ( pm_f(0x3e2aaa8f) + t * pm_f(0xbb355215) )
  if k == 0: return 1.f - ( (r * c) / (c - 2.f) - r )
  y = 1.f - ( (lo - (r * c) / (2.f - c)) - hi )
  if k >= -125:
     if k == 128: return (y * 2.f) * pm_f(0x7f000000)
     return y * pm_f((0x7f + k) << 23)                               // y * 2^k
  return ( y * pm_f((0x7f + k + 100) << 23) ) * pm_f(0x0d800000)     // subnormal results: y * 2^(k+100) * 2^-100

pm_logf(x):          (x > 0 finite assumed; no handling of 0, negative, inf, NaN)                     pm_math.cuh:40-52
  ix = bits(x); k = 0
  if ix < 0x00800000: x = x * 33554432.f; k = -25; ix = bits(x)
  k += (ix >> 23) - 127;  ix &= 0x007fffff
  i = (ix + 0x4afb20) & 0x800000
  m = pm_f(ix | (i ^ 0x3f800000));  k += i >> 23
  f = m - 1.f;  s = f / (2.f + f);  z = s * s;  w = z * z
  t1 = w * ( pm_f(0x3eccce13) + w * pm_f(0x3e789e26) )
  t2 = z * ( pm_f(0x3f2aaaaa) + w * pm_f(0x3e91e9ee) )
  R = t2 + t1;  hfsq = (0.5f * f) * f;  dk = (float)k
  return dk * pm_f(0x3f317180) - ( (hfsq - ( s * (hfsq + R) + dk * pm_f(0x3717f7d1) )) - f )

pm_log1p_pos(y):     (y >= 0)                                                                         pm_math.cuh:54-58
  u = 1.f + y
  c = (y <= 1.f) ? y - (u - 1.f) : 1.f - (u - y)
  return pm_logf(u) + c / u

pm_silu(x)     = x   / (1.f + pm_expf(-x))                                                            pm_math.cuh:59
pm_sigmoid(x)  = 1.f / (1.f + pm_expf(-x))                                                            pm_math.cuh:60
pm_softplus(x) = x > 20.f ? x : pm_log1p_pos(pm_expf(x))                                              pm_math.cuh:61
```
The rest of the file (`pm_exp_d`, `pm_log_d`, `pm_ksin`, `pm_kcos`, `pm_sincos_f`, lines 63-150) is double precision: used by the
sampler and by the RoPE table (`q35_rope_table_kernel`, `q35_kernels.cu:1096-1102`), i.e. the attention / sampling specs.
These are fdlibm-style (`e_expf.c`, `e_logf.c`) and deterministic on any IEEE machine without contraction.

---------------------------------------------------------------------------------------------------

## 3. The a8 tail: int8 quantisation of the next GEMM's input

Shared helper `a8_group512_tail(buf, s_amax, qx, alpha, lo_tok)` (`a8_prologue.cuh:122-133`). Every producer first fills
`buf[i] = value[i] * (float)signs_in[blk*512 + i]` for one block of 512 inputs of one row, then:

```
t[0..511] = fwht512(buf)                                   // §2.3, unnormalised
v[i]      = round_f16( t[i] * K512 )                       // fp16 rounding (NOT bf16)
amax      = max_i |v[i]|                                   // compared as uint bit patterns of fabsf; same as float max
alpha     = amax / 127.f                                   // one f32 per (row, block of 512)
q[i]      = alpha > 0 ? clamp( rint_half_even( v[i] / alpha ), -127, 127 ) : 0          // __float2int_rn, int8
```
Decisive lines:
```
const float v = round_f16(__fmul_rn(fwht512_v(buf[tid], buf, fw_tmp), 0.044194173824159216f));
const float al = __fdiv_rn(__uint_as_float(*s_amax), 127.f);
const float r = al > 0.f ? __fdiv_rn(v, al) : 0.f; int q = __float2int_rn(r); q = q < -127 ? -127 : (q > 127 ? 127 : q);
```
So the GEMM sees `x_rot[i] ≈ alpha[blk] * q[i]`, where `x_rot = H512( x ⊙ signs_in ) / sqrt(512)` blockwise.
The register twin `q35_wa8_tail` (`q35_kernels.cu:261-272`) is identical.

### 3.1 Plain layout
`qx[tok*N + blk*512 + i]`, `alpha[tok*(N/512) + blk]`.

### 3.2 "lo" layout (default when M ≤ 8; `lo_now_`, `engine35.cpp:827`)
`lo_now_ = opt_lo_ && xsmc_ && M <= 8 && !opt_flow_ && !(opt_fin_ && M == 1) && !(chk_on_ && !chk_.empty())` → true by default for M ≤ 8.
Each block of 512 inputs owns 4096 bytes holding 8 rows interleaved (`a8_prologue.cuh:121`):
```
qx[ blk*4096 + a8_lo_off(i, tok) ],   a8_lo_off(i, t) = ((i >> 8) << 11) | (((i >> 4) & 3) << 9) | (t << 6) | (((i >> 6) & 3) << 4) | (i & 15)
```
i.e. bytes are ordered `[i bit 8][i bits 5:4][row t (3 bits)][i bits 7:6][i bits 3:0]`. `alpha` keeps the plain layout.
Rows ≥ M of the 8 are simply not written. Writers that honour `lo`: `q35_res_norm_xsm`, `q35_gdn_out`, `q35_mlp`, `q35_combine`
(`A.lo ? A.qx + (size_t)blk * 4096 : A.qx + (size_t)tok * N + blk * 512`, last argument `A.lo ? tok : -1`).
The GEMM is told with `P.lo = 1` (`engine35.cpp:409`); the head with `P.qx = qx_; P.lo = 1; P.lo_t0 = row0` (`engine35.cpp:807`).

---------------------------------------------------------------------------------------------------

## 4. XSUM contract between the GEMM and the post kernels (default mode 0, M ≤ 16)

For every output `n` of every row `tok` the GEMM leaves an exact **int64** `acc[tok*N + n]` in the tensor's `part` buffer and one
scale per row `us[tok]`:
```
us[tok] = ldexpf( max_g alpha[tok][g], -24 )                                   // lnord.cu:53, 258
A_g     = rint( alpha[tok][g] * (16777216.f / max_g alpha[tok][g]) )            // lnord.cu:54   (GEMM spec)
acc     = sum_g A_g * sum_{i in block g} w8[n][i] * q[i]                        // integer, order-free
```
Reader (`xsum_combine`, `a8_prologue.cuh:10-13`) — **reads and zeroes**:
```
long long* p = (long long*)part + (size_t)tok * N + n;  const long long s = __ldcg(p); __stcg(p, 0ll); return s;
```
The CUDA GEMM accumulates into this buffer, so it must be zero before each launch; the post kernel restores that. In SYCL either
keep the read-and-clear or let the GEMM overwrite. `reset()` also zeroes the buffers (`engine35.cpp:395-397`).

```
y(acc, us)   = (float)acc * us                 // __ll2float_rn = int64 -> f32 round-nearest-even;   a8_prologue.cuh:15
```
RMS factor of the row whose normalised vector fed the GEMM (`q35_xsm_r`, `q35_kernels.cu:81-85`):
```
s = 0; for b in 0..9: s = s + S_[tok*16 + b]           // sequential
r(tok) = q35_rsqrt( s / 5120.f + eps )                 // eps = 1e-6 inside the square root, added AFTER the division
```
Two kinds of GEMM in a layer:
* **normed** inputs (`gemv(..., normed = true)`): qkv, z, q, k, v, gate, up, and the head. Their input was `h*(1+w)` *without* the
  1/rms factor, so the consumer multiplies by `r`: `val = ( y(acc, us) * r ) * tau[n]`.
* **plain** inputs: out_proj, o_proj, down_proj. Consumer (`res_norm_xsm`): `val = y(acc, us) * tau[n]`.

`us_` and `S_` are single engine-wide buffers: `us_` is overwritten by every GEMM launch, `S_` by every `res_norm_xsm`.
The order of kernels in §0.2 guarantees each consumer sees the right ones.

(Modes 1 / 3 for M > 16: `part` then points into `ybuf_` and already holds the f32 `y(acc,us)` (mode 1) or `y(acc,us)*r` (mode 3),
`engine35.hpp:253-255`, `q35_kernels.cu:89-101`, `pfa.cu:302-308`. See §10.3.)

---------------------------------------------------------------------------------------------------

## 5. Residual stream (default = xsm variants; definite)

`Engine35::res_norm` (`engine35.cpp:559-573`) with `xsmc_` true always launches **`q35_res_norm_xsm_kernel`**
(`q35_kernels.cu:1427-1467`); `launch_q35_res_norm` / `_row` / `launch_q35_residual` are unreachable in the default configuration.

There is **no separate norm kernel**. One kernel does "add the sublayer output to h, then prepare the normalised input of the next
sublayer". It is called 2× per layer plus once for the embedding:

| call site | `part` (added to h) | `w` (norm weight of the NEXT sublayer) | `sg_in` | `xu` |
|---|---|---|---|---|
| `pass_body`, `engine35.cpp:829` | none; h := embedding | `L[0].ln1` | `L[0].qkv.signs_in` (or `q`) | `xn_` if layer 0 is GDN |
| `layer` GDN, `:793` | `out_proj` | **`ln2`** of this layer | `gate.signs_in` | null |
| `attn_block`, `:736` | `o_proj` | **`ln2`** | `gate.signs_in` | null |
| `layer`, `:798` | `down_proj` | **`ln1` of layer l+1** | next `qkv.signs_in` / `q.signs_in` | `xn_` if next layer is GDN |
| `layer`, `:799` (l = 63) | `down_proj` | **`fnorm_`** (final norm) | `head_.signs_in` | null (MTP off) |

So: pre-norm architecture, `h += attn(ln1(h))`, `h += mlp(ln2(h))`, logits = head(fnorm(h)); h is updated **only** in this kernel.

### 5.1 Embedding (`q35_embed_blk_kernel`, `q35_kernels.cu:1527-1556`; default because `YSKN_Q35_EMB1` is unset)
The table is coded (`embed_tokens`: role embedding, layout row, bits K = 6, L = 12, start zero, H512 rotation; no `.res` tables in the
speed model, so `codes2/codes3` are null). Grid (10 blocks of 512, M rows), 256 threads. For token `id = ids[row]`:
```
wps = 4*K = 24 uint32 words per sequence; a row = 20 sequences of 128 steps (256 values each)
row_ptr = codes + id * 20 * wps;   sc = h2f(scale16[id])                       // fp16 scale per token
for q in 0..19, j in 0..127:
    w = row_ptr + q*wps;   e = K*(j+1)
    s = (e >= 12) ? bits(w, e-12, 12) : ( bits(w, 0, e) << (12 - e) )          // 12-bit trellis state, LSB-first bit stream
    x[q*256 + 2j]   = h2f(lut16[2s])   * sc
    x[q*256 + 2j+1] = h2f(lut16[2s+1]) * sc                                    // lut16: 4096 x 2 fp16
for b in 0..9:  t = fwht512( x[b*512 .. b*512+511] )
    emb_rows[row*5120 + b*512 + i] = f2bf( (t[i] * K512) * (float)emb_signs[b*512 + i] )
```
`bits(w, start, width)` = `q35_read_window` (`q35_kernels.cu:1496-1500`): `v = w[start>>5] >> (start&31)`, OR in `w[idx+1] << (32-sh)`
if it straddles, mask to `width`. `emb_signs` = `sign_stream_rule(rot_in.rule, seed, offset, 5120)` (`engine35.cpp:284-285`).
`emb_src` then passes `emb = emb_rows_`, `ids = iota` (`engine35.cpp:584-588`), so `h = bf2f(emb_rows[row][n])`.

### 5.2 `q35_res_norm_xsm_kernel` (grid: 10 blocks × M rows, 512 threads; one thread = one element n = blk*512 + tid)
Args `Q35XsmResArgs` (`q35.hpp:138-141`): `part, us, tau, sg_out, emb, ids, M, h, w, sg_in, qx, alpha, S, mode, nw, xu, pf, lo`.
```
# 1. new residual value
if part:                                    # mode 0
    acc  = xsum_combine(part, tok, n)                      # int64, read-and-clear
    buf[i] = ( (float)acc * us[tok] ) * tau[n]             # NO r here (out/o/down inputs are not normed)
    y    = ( fwht512(buf)[i] * K512 ) * (float)sg_out[n]
    hn   = rbf( h[tok][n] + rbf(y) )                       # h is bf16-valued f32; y is rounded to bf16 BEFORE the add
else:
    hn   = bf2f( emb[ ids[tok]*5120 + n ] )
h[tok][n] = hn

# 2. per-block sum of squares of the NEW h (this is all that is stored of the norm)
for t in 0..127:  a_t = 0;  for j in 0..3: a_t = a_t + hn[blk*512 + t + 128*j] * hn[blk*512 + t + 128*j]     # sequential in j
w_k  = xor_tree32( a_{32k .. 32k+31} )   for k = 0..3
S[tok*16 + blk] = seq4(w_0, w_1, w_2, w_3)

# 3. next sublayer's input: weight applied, RMS factor deferred
u = hn * ( 1.f + bf2f(w[n]) )                              # f32, NOT rounded
if xu: xu[tok*5120 + n] = f2bf(u)                          # bf16 copy for the a/b projections
buf[i] = u * (float)sg_in[n]
a8 tail (§3) -> qx, alpha for the next GEMM
```
Decisive lines (`q35_kernels.cu:1438-1463`):
```
buf[tid] = __fmul_rn(q35_xsm_fin(acc, A.mode, nrs.x), c_tau); ... q35_inv_h512s(buf, (float)c_so);
hn = rbf(__fadd_rn(c_h, rbf(buf[tid])));
A.S[(size_t)tok * Q35_SST + blk] = sb;
const float u = __fmul_rn(hn, __fadd_rn(1.f, bf2f(c_w)));
if (A.xu) A.xu[(size_t)tok * Q35_H + n] = f2bf(u);
buf[tid] = __fmul_rn(u, (float)c_si);
```

### 5.3 The RMS norm as it is effectively computed
Reference formula `norm(h)[n] = h[n] * rsqrt(mean(h^2) + eps) * (1 + w[n])`. The engine factors it:
* `u[n] = h[n]*(1+w[n])` is quantised and multiplied by the weights;
* `r = rsqrt( (Σ_{b<10} S[b]) / 5120 + eps )` (eps **inside** the sqrt, after the mean) is multiplied into the GEMM output by the
  consumer (§4) — valid because the GEMM is linear. For the a/b projections it is multiplied into the dot product (§6.1).

### 5.4 norm_offset
The header has `"norm_offset": 1` (written by `yskn_convert.py:951`); the engine never reads that key — the convention is hard-coded:
**the stored vector is `w` and the kernels use `1 + w`** (the checkpoint itself stores zero-centred weights; they are copied unchanged).
`1 + bf2f(w)` is used for `ln1`, `ln2`, `fnorm_`, and the attention `q_norm` / `k_norm` (`q35_kernels.cu:1046`).
**`linear_attn.norm` (gnorm) is used as stored, without the +1** (`q35_kernels.cu:976`: `rbf(__fmul_rn(bf2f(c_gw), ...))`).

---------------------------------------------------------------------------------------------------

## 6. Gated DeltaNet layer (`Engine35::layer`, `engine35.cpp:768-794`)

Inputs of the layer: `qx/alpha` = quantised `u = h*(1+ln1)` (from §5.2), `xn_` = bf16 `u`, `S_` = block sums of squares of h.

```
gemv({qkv, z}, M, normed)           # in_proj_qkv: 5120 -> 10240, in_proj_z: 5120 -> 6144 (both from the same qx)
launch_q35_mix (with_ab = 1 when M <= 32)
launch_q35_conv; launch_q35_state; launch_q35_gdn_out
gemv_res(out, ln2, gate.signs_in)   # out_proj: 6144 -> 5120
```

### 6.1 a / b projections (`q35_ab_kernel`, `q35_kernels.cu:746-787`; fused twin `q35_ab_row1`, `:789-821`, run as the first 96 blocks of the mix kernel)
Dense bf16 matrices `wb`, `wa` (48 × 5120 each), **not** compressed. Output `ab[tok*96 + r]`: r in 0..47 → row r of `wb` (b),
r in 48..95 → row r-48 of `wa` (a).
```
for t in 0..127:                               # thread t owns elements i = 8*(t + 128*j) + e,  j = 0..4, e = 0..7
    acc_t = 0
    for j in 0..4: for e in 0..7:              # sequential, in this order
        i = 8*(t + 128*j) + e
        acc_t = acc_t + bf2f(W[r][i]) * bf2f(xn[tok][i])
w_k = xor_tree32(acc_{32k..32k+31}), k = 0..3
sum = seq4(w_0..w_3)
ab[tok*96 + r] = rbf( sum * r(tok) )           # r(tok) = q35_xsm_r(S_ + tok*16, eps), §4;   A.S set since xsmc_ (engine35.cpp:774)
```
(`__uint_as_float(wa[e] << 16)` is the low u16 of a 32-bit word, `& 0xffff0000` the high one: plain element order.)
So `b = rbf(wb · norm1(h))`, `a = rbf(wa · norm1(h))` with `norm1(h) = u * r`, `u` rounded to bf16.

### 6.2 `q35_mix_kernel` (`q35_kernels.cu:823-858`): GEMM outputs → `mixed` and `z`
Grid: `(20 + 12) * M` blocks (+96 for the fused a/b rows) × 512 threads. Blocks 0..19 of a row handle `in_proj_qkv` (N = 10240),
blocks 20..31 handle `in_proj_z` (N = 6144). For element n of its tensor:
```
acc    = xsum_combine(part_X, tok, n)
buf[i] = ( ((float)acc * us[tok]) * r(tok) ) * tau_X[n]                 # normed GEMM
y      = ( fwht512(buf)[i] * K512 ) * (float)sg_X_out[n]
qkv:   mixed[tok*10240 + n] = f2bf(y)            # stored as bf16 u16
z:     z[tok*6144 + n]      = rbf(y)             # stored as f32
```
`mixed` is `in_proj_qkv(norm1(h))`; `z` is `in_proj_z(norm1(h))`. Split of `mixed` (channel index c, `q35_kernels.cu:864-866`):
```
q: c = head*128 + d,            head 0..15   -> channels     0 ..  2047
k: c = 2048 + head*128 + d,     head 0..15   -> channels  2048 ..  4095
v: c = 4096 + head*128 + d,     head 0..47   -> channels  4096 .. 10239
```
`z` is `[48][128]` (head h, dim j at `h*128 + j`), used only for the output gate (§6.6).

### 6.3 Causal conv1d, kernel 4, depthwise (`q35_conv_kernel`, `q35_kernels.cu:861-901`)
Grid: (16 + 16 + 48 slots) × M rows, 128 threads; slot = one head of q / k / v, thread = one channel. For row t, channel c:
```
x(r) = bf2f( mixed[r*10240 + c] )            if r >= 0        # earlier rows OF THIS PASS
     = bf2f( hist[(r + 3)*10240 + c] )       if r <  0        # committed history: hist[2] = previous token, hist[0] = 3 back
w0..w3 = bf2f( conv[c*4 + 0..3] )
acc = ( ( w0*x(t-3) + w1*x(t-2) ) + w2*x(t-1) ) + w3*x(t)     # no bias
act = rbf( q35_silu( rbf(acc) ) )                             # SiLU activation; rounded to bf16 before and after
```
`hist` is zero after `reset()` (`engine35.cpp:398`), so the first tokens see zero padding = causal padding of the reference conv.
The conv is applied to all 10240 channels of `mixed`, i.e. before the split into q / k / v.

### 6.4 q, k, v, beta, g (same kernel)
```
v[t][head][d] = act                                                        # f32 (bf16-valued), 48 x 128

q/k (per head of 128):
   ss  = seq4( xor_tree32(act^2 of warp 0), ..warp 1, ..warp 2, ..warp 3 )     # warp k = dims 32k..32k+31
   y   = act * q35_rsqrt( ss + 1e-6f )                                     # L2 normalisation; literal 1e-6 INSIDE the sqrt
   k[t][head][d] = y                                                       # f32, not rounded to bf16
   q[t][head][d] = y * 0.08838834764831845f                                # extra 1/sqrt(128); f32, not rounded

per value head h (thread 0 of the v slot):
   b = ab[t*96 + h];   a = ab[t*96 + 48 + h]
   beta[t][h] = rbf( q35_sigmoid(b) )
   g[t][h]    = ( -q35_exp( bf2f(A_log[h]) ) ) * q35_softplus( a + bf2f(dt_bias[h]) )      # log-decay, <= 0; f32, not rounded
```
Decisive lines (`q35_kernels.cu:883-886, 894-896`):
```
A.beta[...] = rbf(q35_sigmoid(b));
const float sp_in = __fadd_rn(a, bf2f(c_dt)); const float sp = q35_softplus(sp_in);
A.g[...] = __fmul_rn(-q35_exp(bf2f(c_al)), sp);
const float y = __fmul_rn(act, q35_rsqrt(__fadd_rn(tot, 1e-6f)));
if (kind == 0) A.q[...] = __fmul_rn(y, 0.08838834764831845f); else A.k[...] = y;
```
The decay actually applied is `exp(g)` (computed in the state kernel).

### 6.5 State recurrence (`q35_state_kernel`, `q35_kernels.cu:905-959`)
State per value head h: `S_h ∈ R^{128×128}`, `S[h][i][j]`, i = key dim, j = value dim. Value head h uses key head `kh = h / 3`
(`kh = h / (Q35_GV / Q35_GK)`): each of the 16 q/k heads is shared by 3 consecutive value heads.
Grid (48 heads, 4 column groups) × 128 threads; thread (warp w, lane) holds the 32 rows `i = 32w .. 32w+31` of column `j`.

Rows are processed **sequentially t = 0 .. M-1 inside the kernel**, starting from the committed state:
```
Sloc = S[h]                                              # local copy (registers)
for t in 0..M-1:
    kt = k[t][kh][:], qt = q[t][kh][:], vt = v[t][h][:], bt = beta[t][h]
    eg = q35_exp( g[t][h] )
    for each column j:
        # (1) decay, then read with k
        for w in 0..3:  kv_w = 0
            for r in 0..31 (sequential):  i = 32w + r
                Sloc[i][j] = Sloc[i][j] * eg
                kv_w = kv_w + Sloc[i][j] * kt[i]
        kv = seq4(kv_0, kv_1, kv_2, kv_3)
        # (2) delta rule
        d = ( vt[j] - kv ) * bt
        # (3) write, then read with q
        for w in 0..3:  o_w = 0
            for r in 0..31 (sequential):  i = 32w + r
                Sloc[i][j] = Sloc[i][j] + kt[i] * d
                o_w = o_w + Sloc[i][j] * qt[i]
        core[t][h][j] = rbf( seq4(o_0, o_1, o_2, o_3) )
    if t + 1 == n_commit:  S[h] = Sloc                   # store the state after row n_commit-1
```
In matrix form: `S ← e^{g_t} S;  S ← S + k_t ⊗ ( β_t (v_t − Sᵀ k_t) );  o_t = Sᵀ q_t` — the standard gated delta rule
(q already carries 1/sqrt(128)). `S` itself is full f32 (never rounded). Decisive lines (`:941, 944, 947, 949-951`):
```
for (r...) { s[r] = __fmul_rn(s[r], eg); kv = __fadd_rn(kv, __fmul_rn(s[r], sk[pb][warp * 32 + r])); }
const float d = __fmul_rn(__fsub_rn(vj, __fadd_rn(__fadd_rn(__fadd_rn(skv[pb][0][lane], skv[pb][1][lane]), skv[pb][2][lane]), skv[pb][3][lane])), bt);
for (r...) { s[r] = __fadd_rn(s[r], __fmul_rn(sk[pb][warp * 32 + r], d)); o = __fadd_rn(o, __fmul_rn(s[r], sq[pb][warp * 32 + r])); }
if (t + 1 == A.n_commit) { ... Sp[(size_t)(warp * 32 + r) * Q35_GD + j] = s[r]; }
```
The same kernel also advances the conv history when `n_commit > 0` (`:916-925`), elementwise over the 10240 channels, n = n_commit:
```
for k in 0..2:  r = n - 3 + k;   new[k] = (r >= 0) ? mixed[r*10240 + c] : old_hist[(3 + r)*10240 + c]
hist[k] = new[k]                    # i.e. hist := the last 3 of (old hist rows, mixed rows 0..n-1)
```

### 6.6 Output: per-head RMS norm, z gate, quantise (`q35_gdn_out_kernel`, `q35_kernels.cu:962-984`)
Grid 12 blocks × M rows, 512 threads; element e = blk*512 + tid of the 6144-vector, head = e >> 7, j = e & 127 (4 heads per block).
```
c    = core[tok][e]
ss_h = seq4( xor_tree32(c^2 over dims 0..31), 32..63, 64..95, 96..127 )         # per head
r_h  = q35_rsqrt( ss_h / 128.f + eps )                                          # eps = model eps (1e-6), inside the sqrt
n2   = rbf( bf2f(gnorm[j]) * rbf( c * r_h ) )                                   # weight WITHOUT +1, same 128 weights for all heads
out  = rbf( n2 * q35_silu( z[tok][e] ) )                                        # gate: norm(core) * silu(z); silu(z) not rounded
buf  = out * (float)out_proj.signs_in[e]
a8 tail -> qx (plain: qx[tok*6144 + blk*512 + i]; lo: qx[blk*4096 + a8_lo_off(i, tok)]), alpha[tok*12 + blk]
```
Then `gemv(out)` and `res_norm_xsm(part = out, w = ln2)` add `out_proj(out)` to h (§5).

### 6.7 `n_commit`, multi-row passes, replay
`layer(l, M, commit ? M : 0)` (`engine35.cpp:831`): `n_commit` is either M (committing pass) or 0 (speculative pass).

* **Rows of one pass are a causal chain.** Row t sees rows 0..t-1 of the same pass: in the conv through `mixed[r]`, r ≥ 0; in the
  recurrence through the local state `Sloc`; (in attention through the KV rows written at `pos + r`). This holds for every pass,
  committed or not. `core[t]` of every row is always produced.
* **Committing pass** (`n_commit = M`): decode (M = 1, `engine35.cpp:1271`, `pass_body(1, true, nlayers_, HEAD_LAST, false, false, true)`), prefill chunks, `nll`. After the pass
  `S` = state after the last row and `hist` = last 3 `mixed` rows.
* **Speculative pass** (`verify`, `n_commit = 0`, `engine35.cpp:1305`): neither `S` nor `hist` is written. Because M ≤ 16 = `HM_`
  and `opt_kconv_`, the conv writes q/k/v/beta/g into the **per-layer** buffers `Y.tq, Y.tk, Y.tv, Y.tbeta, Y.tg`
  (`keep`, `engine35.cpp:785-786`) and `mixed` stays in the per-layer `Y.pmix`.
* **`commit(n_accept)`** → `replay_commit(rows = n_accept + 1)` (`engine35.cpp:843-866, 1319-1330`): for every GDN layer
  `launch_q35_state({Y.tq, Y.tk, Y.tv, Y.tbeta, Y.tg, Y.S, M = rows, n_commit = rows, core_, Y.pmix, Y.hist})` — the recurrence is
  simply re-run for the accepted prefix from the old `S`, now storing `S` and shifting `hist`. `core_` output is scratch.
  No GEMM is repeated. (Without `opt_kconv_` the conv would be re-run first, `:850-851`.) In CUDA this runs on a second stream;
  it must finish before the next pass reads `S`/`hist` (`replay_fence`).
* A 1-row verify inside the speculative loop commits directly (`c1`, `engine35.cpp:1303-1305`; `pre_committed_`).
* Tree-shaped verify passes (side candidates, `wk` table) use `wk_conv_kernel` / `wk_state_kernel`; see §10.4.

---------------------------------------------------------------------------------------------------

## 7. MLP (`mlp_block`, `engine35.cpp:757-767`; `q35_mlp_kernel`, `q35_kernels.cu:1402-1425`)

Input `qx/alpha` = quantised `u = h*(1+ln2)` from the `res_norm_xsm` after out/o; `S_` = block sums of that h.
`gemv({gate, up}, M, normed)`: gate_proj, up_proj: 5120 → 17408 each. Kernel grid 34 blocks × M rows, 512 threads:
```
ag = xsum_combine(part_g, tok, n);  au = xsum_combine(part_u, tok, n)
vg = ( ((float)ag * us[tok]) * r(tok) ) * tau_g[n];      vu likewise with tau_u
g  = rbf( ( fwht512(vg)[i] * K512 ) * (float)sg_g[n] )           # gate_proj output, bf16
u  = rbf( ( fwht512(vu)[i] * K512 ) * (float)sg_u[n] )           # up_proj output, bf16
x  = rbf( rbf( q35_silu(g) ) * u )                               # SiLU-gated: silu(gate) * up, rounded after silu and after the product
buf = x * (float)down.signs_in[n]
a8 tail -> qx (plain: qx[tok*17408 + blk*512 + i]; lo: blk*4096 + a8_lo_off), alpha[tok*34 + blk]
```
Decisive lines (`:1412, 1416-1419`):
```
vg = __fmul_rn(__fmul_rn(q35_xsm_fin(ag, A.xsm.mode, nrs.x), nrs.y), c_tg);
const float g = rbf(__fmul_rn(__fmul_rn(vg, 0.044194173824159216f), (float)c_sg));
const float x = rbf(__fmul_rn(rbf(q35_silu(g)), u));
bu[tid] = __fmul_rn(x, (float)c_si);
```
Then `gemv({down})` (17408 → 5120) and `res_norm_xsm(part = down, w = next ln1 | fnorm)` (§5): `h += down(silu(gate)*up)`.
`mlp_block` is called with `down = false`; the down GEMM is launched by `gemv_res` (`engine35.cpp:671, 798-799`).

---------------------------------------------------------------------------------------------------

## 8. Final norm, head, argmax

### 8.1 Final norm
Not a kernel of its own. The last `res_norm_xsm` (layer 63, `engine35.cpp:799`) is called with `w = fnorm_`, `sg_in = head_.signs_in`:
it writes `qx/alpha` = quantised `h*(1 + fnorm)` and `S_` = block sums of squares of the final h. `r` is applied in the argmax kernel.
`h_` after the pass is the **un-normalised** final hidden state (`yskn_hidden_rows` returns it, `engine35.cpp:1332`).

### 8.2 `head_slice(row0, rows, advance, topk, nll)` (`engine35.cpp:803-823`)
`pass_body`: `HEAD_LAST` → `head_slice(M-1, 1, ...)`; `HEAD_ALL` → slices of `HM_ = 16` rows; `HEAD_NONE` → nothing (`:836-837`).
GEMM input of the slice: `qx_ + row0*5120`, `alpha_ + row0*10` (lo: `qx_`, `lo_t0 = row0`). GEMM output: `head_.part` int64
`[rows][V]`, `us_[0..rows-1]` (slice-relative). No output rotation, no `signs_out`.

### 8.3 `argmax_stage1<XSM=true>` (`misc_kernels.cu:57-89`), grid 256 blocks × rows, 256 threads
```
r_row  = rsqrtf( (Σ_{b=0..9} S_[(row0+row)*16 + b]) / 5120.f + eps )           # plain CUDA rsqrtf even in pm mode (misc_kernels.cu:65)
logits[row][v] = ( ((float)acc[row][v] * us[row]) * r_row ) * head.tau[v]      # f32, V = 248320; also clears acc
next[row0 + row] = argmax_v logits[row][v],   ties -> the smallest v           # x > best || (x == best && v < bi)
```
Arguments: `AA.rr = S_ + row0*Q35_SST; AA.xsm_nb = H_/512 (10); AA.xsm_stride = 16; AA.xsm_n = 5120; AA.xsm_eps = eps_`.
`logits_` holds the last slice only (`yskn_logits` = row 0 of it, `yskn_logits_rows`). Result goes to `d_next8_[row0 + row]`.
With `advance` (decode) and no sampler: `*d_pos += 1; *d_toks = argmax` on the device, which feeds the next 1-row pass.
The 2-stage block reduce (`blk_val/blk_idx`, `cnt`) is an implementation detail; any exact argmax with lowest-index tie-break is equal.

### 8.4 topk8 (`misc_kernels.cu:100-147`) and nll (`:150-166`)
* topk8: the 8 best `(value, index)` of each row of `logits_`, ordered by `value desc, index asc` (`tk_better`); to
  `top_val_/top_idx_[(row0+row)*8 + k]`. Used by `verify` / `prefill_rows` when the sampler does not provide them.
* nll: `out[row] = (float)( log( Σ_v exp((double)l[v] - (double)max) ) + max - l[target[row]] )` in double; per-thread partial sums
  over `v ≡ thread (mod 256)` then a pairwise tree. Diagnostic (`yskn_nll`), not needed for generation.
* With sampling on (`smp_`), `launch_q35_smp` runs after the argmax on the same `logits_` (`q35_sample.cu`, not this spec).

---------------------------------------------------------------------------------------------------

## 9. Capture and the remaining files

* **Capture** (`pass_body`, `engine35.cpp:832-833`): after layer `l`, if `l` is in `cap_layers_`, `memcpy(cap_ + k*Pmax_*5120, h_, M*5120*4)`.
  The captured tensor is the f32 residual stream **after** the layer's `down` residual add (i.e. the input h of layer l+1, before
  any norm). `yskn_set_capture(layers)` / `yskn_capture_rows(k, n, out)` (`engine35.cpp:546-549`). This is the validation oracle for
  Milestone A and the feature source of the draft model.
* **`q35_prefill.cu`** (22 lines): `capture_a8_kernel` — takes captured hidden rows (f32 `[rows][5120]`), multiplies by the draft
  model's input signs of capture slot `slot` (plain `x * sign`), runs the a8 tail, writes `qx[((row0+m)*count + slot)*5120 + ...]`,
  `alpha[((row0+m)*count + slot)*10 + g]`. Used only by the layer-wise prefill to hand hidden states to the drafter
  (`engine35.cpp:1116`). Not needed before Milestone D.
* **`q35_g6.cu`**: prefill-only GEMM ("mt_gemm6"). For passes with more than 16 rows it expands one matrix at a time to int8
  (`mt_predecode2_kernel`) into a temporary workspace and reuses it for all rows (`TECHNOLOGY.md` §8 last paragraph;
  `[q35_g6] prefill only`, `engine35.cpp:314-321`; dispatched from `gemv` when `xsmc_ && M > 16`, `engine35.cpp:413-416`). It writes
  f32 `y = (float)acc * us` (× `r` for normed GEMMs) into `ybuf_`. GEMM spec; not needed if prefill is done in chunks of ≤ 16 rows.
* **`q35_chk.cu`**: optional runtime self-check of a GEMM (`yskn_chk_*`). Recomputes a random linear combination
  `Σ_r rho(key, r) * acc[r]` and compares with `Σ_i A_g*q[i]*c[i]` (Freivalds-style, mod 2^64) plus a range bound; has a fault
  injector. Off unless a check vector is registered (`chk_on_`). Ignore; note that an active check disables "lo".
* **`misc_kernels.cu`**: besides argmax / topk8 / nll: `embedding_kernel` (other engine), `advance_kernel`, `set_int(s)_kernel`
  (write `d_pos_`, `d_toks_`), `verify_prologue_kernel`, `prefetch_kernel`, `hist_spec_kernel` (copies capture rows into a history
  ring for the old drafter). Trivial or unused by Engine35's default path.
* **`post_kernels.cu`**: other engine (see §0.3).
* **`q35_flow.cu`**: FLOW mode (§10.2).
* Small Engine35 helpers in `q35_kernels.cu`: `q35_iota` (`out[i] = i`), `q35_pos_slices` (`out[i] = *base + i*step`),
  `q35_pick`, `q35_mtp_log` (old MTP drafter), `q35_l2pf` (prefetch), `q35_visual_embed` / `q35_visual_rope` (image input).

---------------------------------------------------------------------------------------------------

## 10. Non-default variants (reference only)

### 10.1 Non-XSUM `q35_res_norm_kernel` (`q35_kernels.cu:119-164`), `_row` (`:169-217`), `q35_residual` (`:218-227`)
Used only with `YSKN_Q35_XSUM=0`. `part` holds f32 partial sums of `nw` workers, combined sequentially (`q35_combine`, `:48-70`).
```
y  = (fwht512( tau[n] * Σ_w part_w[tok][n] ) * K512) * sg_out[n];     h = rbf( h + rbf(y) )      (or h = bf2f(emb) for the first call)
tot = seq4 of xor_tree32 of per-thread sums: thread t (0..127) sums x[t + 128*j]^2 sequentially, j = 0..39
r  = q35_rsqrt( tot / 5120.f + eps )
v  = rbf( (h[n] * r) * (1.f + bf2f(w[n])) );   xn = f2bf(v);   buf = v * sg_in[n];   a8 tail
```
Here the normalised value **is** rounded to bf16 before quantisation and the GEMM consumers do not multiply by `r`.
The `cnt` atomics are a cross-block barrier (all 10 blocks must have written h before the row sum). `_row` = one block per row for
M > 64; `q35_residual` = the residual add alone for 16 < M ≤ 64.

### 10.2 `flow_mlp` (`engine35.cpp:738-756`, `q35_flow.cu`), `fin`
FLOW (`YSKN_Q35_FLOW=1`, M ≤ 8): one persistent kernel per layer that runs gate/up GEMM, the §7 math, the down GEMM and the §5.2
res-norm from a precomputed work schedule. FIN (`YSKN_Q35_FIN`, M = 1): the res-norm is fused into the tail of the GEMM kernel and
writes `qx2_/alpha2_` (`in2_`). Both are meant to be numerically the same pipeline; both off by default.

### 10.3 M > 16 (prefill) path
* `nmode(M) = 1`, `nmode(M, true) = 3`; `pt()` redirects `part` to `ybuf_` (`engine35.hpp:253-255`). The GEMM (`q35_g6` / `pfa`)
  writes f32 `y`: mode 1 `(float)acc * us`; mode 3 `((float)acc * us) * rr`, `rr = rsqrtf(Σ S/5120 + eps)` (`pfa.cu:161-163, 302-308`).
  Consumers then use `buf = y * tau[n]` (`q35_kernels.cu:843, 1410`) — the same operations in the same order as mode 0.
* 32 < M: the a/b projections run as their own kernel (`launch_q35_ab`, `engine35.cpp:775`) instead of fused in mix.
* M > 64 with `YSKN_Q35_PFALT=1` (shipping): the `*_pf_kernel` twins. I compared each with its M ≤ 64 twin: same formulas and the
  same reduction orders (lane `l`, slot `i` ↔ element `l + 32*i`), including `q35_state_pf_kernel` / `q35_state_nb_kernel`.
* M > 64: `pmix_big_`, `pab_big_` replace the per-layer `Y.pmix`, `Y.pab` (`engine35.cpp:771`).
* `prefill()` uses `prefill_vsp` (layer-by-layer over thousands of rows, `YSKN_Q35_LAYER_ROWS=4096`) when n ≥ 1024, else
  `prefill_chunks` with `PM_ = 1024` rows per committing pass (`engine35.cpp:982-987, 1097, 1200-1216`).
* **For the port**: prefill as a sequence of committing passes of ≤ 16 (or ≤ 8) rows through the §5-§8 kernels is the same
  computation, because the int64 sums are exact and every float step is per row. **UNCERTAIN**: I did not run a bit comparison of
  the M > 16 CUDA path against chunks of ≤ 16.

### 10.4 Tree rows (`wk`, `lfrow_kernels.cuh:31-150`) — becomes active with the draft model
`wk_pass(M, commit, head_mode) = opt_lfrow_ && !commit && M <= 16 && head_mode == HEAD_ALL` (`engine35.hpp:364`). `opt_lfrow_` is 0
after open, but `specx.cpp:195-197` switches it on for the generation call when the Bdraft drafter is loaded and HCAP is on
(`bdr_.hb_on` defaults to 1, `engine35.cpp:1693`). Then verify passes use `launch_wk_conv / launch_wk_state` with a 48-byte table
`d_wk_`: `wk[t]` = depth of row t (its position offset), `wk[16 + i]` = i-th step of the recurrence (`row | 0x80` if leaf).
* conv: history rows are taken by depth: `x(r)` for `r = dp-3 .. dp-1` (`dp = wk[t]`) from `mixed[r]` / `hist`, current input `mixed[t]`.
* state: steps are executed in table order; a **leaf** step computes `core[row]` from `eg*S` and the delta update *without*
  modifying the running state (side candidate), a normal step advances it as in §6.5.
With the identity table (`wk_plain_table`, `wk[i] = i`, no leaf bits) this is exactly §6.3-§6.5. Needed for Milestone D only.

---------------------------------------------------------------------------------------------------

## 11. Uncertainties and porting hazards

1. **Transcendentals are not portable in the default configuration.** `rsqrtf`, `expf`, `log1pf` (CUDA libm) are used for: the RMS
   factor `r` (every normed GEMM consumer, a/b, gdn_out, argmax), L2 norm of q/k, SiLU (conv, z gate, MLP), sigmoid (beta),
   `exp(A_log)`, softplus, `exp(g)`. Expect last-ulp differences against the CUDA oracle; most are hidden by the following bf16
   rounding but `r`, q/k and `g` are not rounded. Recommendation: implement §2.5 (pm) in SYCL. To get a bit-comparable CUDA oracle,
   switch pm on there: `yskn_set_opt(e, 21, 1)` or `YSKN_Q35_PMATH=1`. **UNCERTAIN / caveats**: (a) PMATH also selects the `kz`
   attention kernels (`engine35.cpp:708`) and throws with `YSKN_TB_CAP=1` ("does not support PMATH", `:677`); (b) the alternative
   `YSKN_Q35_PM=1` turns the pm flag on without `kz_`, but the same variable is also parsed as the prefill chunk size
   (`PM_ = min(Pmax_, atoi)`, `engine35.cpp:213`), so it forces 1-row prefill passes; (c) the argmax kernel always uses `rsqrtf`.
   I did not run either configuration.
2. **icpx fast-math / contraction**: needs `-fp-model=precise -ffp-contract=off`; `int64 → float` must be round-to-nearest-even;
   `rint` must be half-to-even; half conversion must be RNE with overflow to inf.
3. `tau`, `signs_in/out`, `us`, the worker-count details and the "lo" reader are owned by the GEMM spec; I only state how the
   kernels here consume them. The exact definition of `tau` (`loader.cpp:19-24`, fp16 row scale × LUT gain) was not traced.
4. `norm_offset`: decided from the kernels (`1 + w` everywhere except `linear_attn.norm`). The header key is not read by the
   engine; if a different model had `norm_offset = 0` the engine would be wrong, so this is a property of this model family.
5. Whether "lo" rows ≥ M of a block must be zero for the GEMM: the writers leave them untouched (**UNCERTAIN**, GEMM side).
6. The M > 16 prefill path and the tree-row (`wk`) path were read but not specified to implementation grade (§10.3, §10.4).
7. `xsum_combine` clears the accumulator; if the SYCL GEMM accumulates across work items it relies on that (§4).
