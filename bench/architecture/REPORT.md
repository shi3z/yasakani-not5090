# Why the engine is slower on the Arc Pro B60 than on the RTX 5060 Ti

Micro-benchmark study of the compressed-GEMM engine on an Intel Arc Pro B60 (24 GB, Xe2, SYCL ESIMD on Level Zero) against
an NVIDIA RTX 5060 Ti 16 GB. Question: both cards have about the same memory bandwidth (456 vs 448 GB/s), yet the B60
decodes about 2.3 x slower and reads prompts about 4 x slower. Sources and raw results are in this directory
(see `README.md`).

**Scope of the evidence.** Everything about the B60 was measured here. The RTX 5060 Ti was busy with another job for the
whole session, so **no 5060 Ti number in this report is a measurement made here**: the CUDA side consists of the vendor
facts, the upstream engine's source, and the machine code nvcc generates for it. The CUDA benchmarks are written and
compile, but have not run.

## 1. Summary

Per launch of the two largest matrices of a layer (gate + up, 2 x 17408 x 5120, 3-bit codes) on the B60:

| | ms | G weights/s |
|---|---|---|
| reading the codes only (435 GB/s, the bus limit) | 0.154 | 1161 |
| + extracting the 12-bit states | 0.185 | 962 |
| + table lookup and int8 operand | 0.267 | 669 |
| + dpas, 1 row | 0.287 | 621 |
| + exact int64 sums = the engine's kernel, 1 row | 0.291 | 614 |
| engine kernel, 8 rows / 16 rows | 0.311 / 0.326 | 574 / 547 |
| engine prefill structure, 32 / 64 / 256 rows | 0.425 / 0.830 / 2.812 | |
| decode-once kernel (experiment), 256 rows | 1.312 | |

What the measurements say about the two symptoms:

1. **Decoding.** The B60's GEMM kernel is limited by memory traffic that does not overlap: the code stream at the full
   bus rate (53 % of the kernel) plus the table lookups (28 %), which cost 0.11 ms per launch regardless of how the kernel
   is written. It is not limited by integer throughput (removing 41 % of the instructions changes nothing), by XMX
   (5 % used), by the 64-bit sums (1 %), by registers (no spills) or by the compiler (IGC and nvcc emit the same number
   of lane-operations per weight). The kernel reaches 53 to 63 % of its bus floor. On top of that, launches that do not
   fill the GPU lose a further 18 to 50 % (the card has 1280 thread slots; `down` uses 320 to 640), and every dependent
   operation in a short kernel costs 10 clock cycles. Of the 38 ms per-pass deficit at most 13 to 24 ms can be GEMM;
   at least 15 ms is outside the GEMM, where this GPU is slow on short, launch-heavy, latency-bound kernels.
2. **Prompt reading.** This is mostly an implementation difference: for 256 rows and more the CUDA engine decodes the
   weights once to int8 and runs a plain tensor-core GEMM over 1024-row chunks; the B60 port decodes every weight once
   per 32 rows and updates the int64 sums in memory after every 512 columns. A B60 kernel that decodes once per
   256 rows and keeps the sums in registers is 2.1 x faster (1.31 ms against 2.81 ms for gate + up; 0.66 against 1.31 ms for down).

Hardware numbers established for the B60 (section 2 has the full facts table):

| quantity | value |
|---|---|
| threads running at once | 1280 (8 per engine); ALU-bound kernels saturate at about 640 |
| integer / float lane throughput | 6.1 x 10^12 lane-ops/s with 16-lane instructions, 11.6 x 10^12 with 32-lane instructions |
| dependent-operation latency / shortest loop trip | 10 cycles / about 50 cycles (2400 MHz) |
| XMX peak | 196 TOPS int8, 98 TFLOPS bf16 (one 16-lane dpas row per 2 cycles per engine) |
| memory read rate | 381 to 448 GB/s on random data, needs about 1000 threads; regular / zero data up to 4 x faster (content dependent) |
| cache levels (dependent 16-lane gather) | up to 256 KiB: 67 ns; second level (to 16 MiB for random data): 155 ns; beyond: 600 ns |
| SLM gather rate | 3.2 x 10^11 lookups/s in a loop; in the GEMM kernel 3.0 x 10^11 |
| registers | 124 live 64-byte values before the first spill |
| launch | 3 us queued, 6 to 7 us with a wait |

## 2. Phase 0: architecture facts

Labels: **[O]** official vendor document or vendor source code, **[3P]** third party (review, database, encyclopedia),
**[M]** measured here (file in `results/`), **[D]** derived by arithmetic from the cited values, **[I]** inferred.
Intel and NVIDIA terms are not interchangeable; the "nearest analogue" column says how they relate.

Sources: ARK = Intel product specification pages of the Arc Pro B60 and Arc B580; NEO = `intel/compute-runtime`
(`shared/source/xe2_hpg_core/hw_info_bmg.cpp`, `gfx_core_helper_xe2_hpg_core.cpp`, generated `hw_cmds_generated_xe2_hpg_core.inl`);
IGC = `intel/intel-graphics-compiler` (`visa/G4_Kernel.cpp`, GRF modes); OGO = Intel oneAPI GPU Optimization Guide 2025-0
(chapters "Intel Xe GPU Architecture", "Thread Mapping and GPU Occupancy", "Small Register Mode vs. Large Register Mode",
"Shared Local Memory"); ESIMD = `intel/llvm` `sycl_ext_intel_esimd.md` and `esimd/xmx/dpas.hpp`; CL-MMA = Khronos
`cl_intel_subgroup_matrix_multiply_accumulate`; L0 = Level Zero specification, core programming guide; NV = NVIDIA RTX 5060
family product page; CUDA-CC = CUDA Programming Guide, appendix "Compute Capabilities"; CUDA-PM = CUDA Programming Guide,
"Programming Model"; PTX = PTX ISA, "Warp-level matrix multiply-accumulate instructions"; press = Notebookcheck /
Chips and Cheese / Tom's Hardware / TechPowerUp / Wikipedia articles relaying vendor slides or their own data.

### 2.1 Execution resources

| item | Arc Pro B60 | label | RTX 5060 Ti 16 GB | label | nearest analogue |
|---|---|---|---|---|---|
| top-level compute block | 20 Xe-cores in 5 render slices | [O] ARK | 36 SMs | [3P] press (no NVIDIA document gives the count) | Xe-core ~ SM |
| execution units | 160 Xe Vector Engines (XVE, "EU"), 8 per Xe-core | [O] ARK; per core [D] | 4608 "CUDA cores" | [O] NV | one XVE executes one 16-lane instruction per cycle; a "CUDA core" is one scalar lane |
| matrix units | 160 XMX engines (one per XVE) | [O] ARK | 144 tensor cores | [3P] | |
| hardware thread contexts | 8 per XVE (`NumThreadsPerEu = 8`), 4 in the 256-register mode: 1280 (640) on the card | [O] NEO, IGC; total [D] | max 48 warps (1536 threads) and 24 blocks resident per SM: 1728 warps on the card | [O] CUDA-CC; total [D] with the 3P SM count | XVE hardware thread ~ warp (not ~ CUDA thread) |
| threads that run at once | 1280: latency-bound kernels gain throughput up to 1280 threads, ALU-bound kernels saturate at about 640 (section 7) | [M] | not measured (GPU in use) | | |
| lanes executing per clock | 160 XVE x 16 = 2560 | [D] | 4608 | [D] from [O] | |
| clock | 2400 MHz graphics clock; the xe driver reported 2400 MHz (`act_freq`) during every run | [O] ARK; [M] | 2407 MHz base / 2572 MHz boost | [O] NV | |

### 2.2 Execution model and registers

| item | Arc Pro B60 | label | RTX 5060 Ti | label |
|---|---|---|---|---|
| model | a hardware thread executes vector instructions of 1..32 lanes; in plain SYCL / OpenCL the compiler maps a sub-group of 8 / 16 / 32 work-items onto one thread; in ESIMD one work-item is one hardware thread and the program holds the vectors explicitly | [O] OGO, ESIMD | SIMT: scalar instruction stream per thread, 32 threads (a warp) execute the same instruction together | [O] CUDA-PM |
| SIMD width chosen by the compilers | plain SYCL kernels of `arith.cpp`: SIMD32 (`simd32_entry`); ESIMD kernels: the width written in the source (16 here) | [M] asm dump | 32 (warp) | [O] |
| native data path | 512-bit XVE, "SIMD16 native ALUs that also support SIMD32 operations" | [3P] press relaying Intel slides | n/a | |
| registers per thread | 128 GRF (default) or 256 GRF (large mode, halves the threads per XVE) | [O] IGC, OGO | up to 255 32-bit registers per thread | [O] CUDA-CC |
| register size | 64 bytes (512 bit): a `simd<int32, 16>` occupies exactly one GRF | [O] NEO (`sizeof(GRF) == 64`); [M] asm (`r7:4` = 4 registers for a 256-byte load) | 4 bytes | [O] |
| register space per thread | 8 KiB (128 x 64 B) | [D] | at most ~1 KiB | [D] |
| register space per lane | 8 KiB / 16 lanes = 512 B = 128 dwords | [D] | 255 dwords | [D] |
| usable before spills | 124 live 64-byte values run without a spill, 128 spill (section 7) | [M] | not measured; nvcc reaches 255 registers at 224 live values and spills at 256 (ptxas log of `cuda/regs.cu`, compile time) | [M] compile only |
| register pool | 8 threads x 8 KiB = 64 KiB per XVE | [D] | 64 K 32-bit registers per SM, shared by the resident threads (more registers per thread = fewer resident threads) | [O] CUDA-CC |

### 2.3 Memories

| item | Arc Pro B60 | label | RTX 5060 Ti | label |
|---|---|---|---|---|
| on-board memory | 24 GB GDDR6, 192-bit, 456 GB/s | [O] ARK | 16 GB GDDR7, 128-bit; 448 GB/s | [O] NV (size, width); [3P] bandwidth |
| sustained bandwidth seen by a kernel | 381 to 448 GB/s reading random data; regular data faster (section 6.1) | [M] | not measured (GPU in use) | |
| work-group local memory | SLM: per work-group, allocated in the Xe-core; driver limit 128 KiB per work-group (`maxProgrammableSlmSize = 128`) | [O] OGO, NEO; [M] 128 KiB kernel ran | shared memory: per block, carved from the SM's unified L1; 48 KiB by default, up to 99 KiB per block with opt-in (cc 12.x) | [O] CUDA-CC |
| L1 | "192 KB of shared L1 cache / SLM" per Xe-core | [3P] press | 128 KiB unified data cache (L1 + shared memory) per SM | [O] CUDA-CC |
| L2 | 18 MB (B580; sources disagree: 12 or 18 MB) | [3P] | 32 MB | [3P] |
| cache steps seen by a kernel | up to about 256 KiB: 67 ns per dependent access; second level, 16 to 32 MiB for random data: about 155 ns; DRAM: 600 ns (section 6) | [M] | not measured | |

### 2.4 Number formats and matrix hardware

| item | Arc Pro B60 | label | RTX 5060 Ti | label |
|---|---|---|---|---|
| FP64 | native (`ftrSupportsFP64 = true`, no emulation); `aspect::fp64` = 1 | [O] NEO; [M] | native; FP32 : FP64 throughput 64 : 1 for cc 12.x | [O] CUDA-CC |
| FP16 | `aspect::fp16` = 1 | [M] | yes | [O] |
| INT64 | add native (one instruction for 16 qwords); 64 x 64 multiply expanded by IGC to 11 instructions of 32-bit `mul` / `mach` / `macl` | [M] asm | 32 x 32 -> 64 multiply-add is one instruction (`IMAD.WIDE`) | [M] SASS |
| matrix instruction | `dpas`: per lane, acc += sum over depth 8 of dot(4 x int8, 4 x int8) (or 2 x bf16 / fp16, 8 x int4, 16 x int2); repeat count 1..8 rows in ONE instruction; 16 lanes | [O] ESIMD, CL-MMA; [M] asm (`dpas.8x8`, execution size 16) | `mma.sync`: a WARP-level instruction; D = A x B + C on fragments spread over the 32 threads; int8: m16n8k32 (also m8n8k16, m16n8k16); bf16 / f16: m16n8k16, m16n8k8; 4-bit and 1-bit forms | [O] PTX |
| int8 multiply-accumulates per instruction | `dpas.8x8` at 16 lanes: 8 rows x 16 lanes x 32 = 4096; `dpas.8x1`: 512 | [D] | m16n8k32: 16 x 8 x 32 = 4096 per warp instruction | [D] |
| operand placement | B (16 lanes x 32 int8) and A (rows x 32 int8, shared by all lanes) and the accumulator are ordinary GRF registers of ONE thread | [O] ESIMD; [M] | fragments are held in registers of all 32 threads of the warp (A: 4 x 32-bit per thread, B: 2 x 32-bit, C/D: 4 x 32-bit) | [O] PTX |
| peak rate claimed | "FP16 2048 and INT8 4096 ops/clock per Xe-core" | [3P] press | "759 AI TOPS" | [O] NV (marketing unit, method not stated) |

### 2.5 Kernel launch model

| item | Arc Pro B60 (SYCL on Level Zero) | label | RTX 5060 Ti (CUDA) | label |
|---|---|---|---|---|
| submission | commands appended to a command list; immediate lists execute on append, regular lists on close + execute; SYCL `queue.wait()` waits for completion | [O] L0 | a launch enqueues a grid of blocks on a stream; `cudaStreamSynchronize` waits | [O] CUDA-PM |
| dispatch unit | `COMPUTE_WALKER` walks thread groups; one work-group = one thread group, placed on one Xe-core (its SLM lives there); ESIMD: one hardware thread per work-item | [O] NEO, OGO, ESIMD | a block runs on one SM; its warps share that SM's shared memory | [O] CUDA-PM |
| threads per group | at most 64 hardware threads per work-group at SIMD16 / SIMD1 with 128 GRF | [O] NEO; [M] an ESIMD work-group of 128 is rejected ("cannot exceed 64") | at most 1024 threads (32 warps) per block | [O] CUDA-CC |
| launch cost | empty kernel: 5.8 to 6.8 us submit + wait, 2.8 to 3.0 us when queued (section 7) | [M] | not measured (GPU in use) | |

Not confirmed by any official source: the SM count, tensor-core count, L2 size and 448 GB/s of the RTX 5060 Ti; the L1 / L2
sizes and the "SIMD16 native" data path of Xe2 (press only); an official FP32 TFLOPS or TOPS figure for the B60.

## 3. Method

* B60: SYCL ESIMD kernels built with the engine's flags (icpx 2026.2, Level Zero, default 128-register mode; the large
  register mode was never used). Every number is the best of 5 repetitions. "queued" = many launches, one wait;
  "sync" = launch + wait each time. Every run took a lock file and waited while other B60 jobs were visible; three
  segments that overlapped another job were repeated (`results/b60_contended.txt`, `results/b60_gemm_stages_repeat.csv`);
  the repeats agree with the first runs within 1 %.
* The xe driver reported 2400 MHz (`act_freq`) in every sample taken during the runs, so 1 cycle = 0.4167 ns.
* The compressed-GEMM kernels compute the engine's exact math on synthetic data of the real shapes and are checked
  bit-exactly against a host reference (`check` column = `exact`). "34816x5120" = gate + up of one layer in one launch
  (2 x 17408 rows, 2176 threads), "5120x17408" = down (320 threads), "20480x17408" = four down matrices (1280 threads,
  used to see the kernel at full occupancy with long threads).
* RTX 5060 Ti: a server process held 15.8 GB and 100 % utilisation for the whole session, so **nothing was run on it**.
  All CUDA programs in `cuda/` compile for sm_120; their index / layout logic was checked on the CPU with an emulation
  shim; they have never executed on a GPU. The CUDA side of this report is therefore static: the upstream engine's
  source and the SASS that nvcc generates for it.
* Labels: [M] measured here, [O] official document, [3P] third party, [D] derived by arithmetic, [I] inferred.

## 4. Decomposition of the compressed GEMM (B60) [M]

File: `results/b60_gemm_stages.csv`. One launch, time in ms (queued). G weights/s = N x Kc / time.

### 4.1 Stages, one input row

| stage (cumulative) | gate+up, 3-bit | gate+up, 4-bit | 4 x down, 3-bit | 4 x down, 4-bit |
|---|---|---|---|---|
| A read the codes | 0.154 (435 GB/s) | 0.203 (439 GB/s) | 0.298 (448 GB/s) | 0.397 (449 GB/s) |
| B + extract the states | 0.185 | 0.211 | 0.331 | 0.403 |
| D + lookup + build the int8 operand | 0.267 | 0.272 | 0.442 | 0.451 |
| E + dpas | 0.287 | 0.297 | 0.477 | 0.497 |
| F + int64 accumulation (= engine kernel) | 0.291 | 0.302 | 0.476 | 0.497 |
| G + scale to float | 0.287 | 0.301 | 0.471 | 0.494 |
| F in G weights/s | 614 | 591 | 749 | 717 |
| F in GB/s of codes | 230 | 295 | 281 | 359 |

(Stage C, "lookup without building the operand", is 0.33 / 0.36 ms, slower than D: its checksum keeps more values alive and
the compiler spills 20 registers per loop body. It is in the CSV but not a valid intermediate point.)

Share of the engine kernel's time (gate+up, 3-bit): **reading 53 %, state extraction 11 %, table lookup + operand 28 %,
dpas 7 %, int64 1 %, float 0 %.** The kernel runs at 53 % (short threads) to 63 % (long threads) of the rate at which the
same kernel structure reads the codes without decoding them.

### 4.2 Rows

| rows M | structure | gate+up 3-bit | gate+up 4-bit | down 3-bit (320 threads x row sets) |
|---|---|---|---|---|
| 1 | small (sums in registers) | 0.291 | 0.302 | 0.247 |
| 8 | small | 0.311 | 0.344 | 0.262 |
| 16 | small | 0.326 | 0.361 | 0.349 |
| 32 | big, 32 rows per thread | 0.425 | 0.462 | 0.436 |
| 64 | big, 32 rows per thread (2 row sets) | 0.830 | 0.891 | 0.450 |
| 64 | big, 16 rows per thread (4 row sets) | 1.237 | 1.329 | 0.556 |
| 64 | big, 64 rows per thread (1 row set, spills) | 1.197 | 1.399 | 1.278 |
| 128 | big, 32 rows per thread (4 row sets) | 1.450 | | 0.660 |
| 256 | big, 32 rows per thread (8 row sets) | 2.812 | | 1.306 |

* Rows are almost free up to 16 (one `dpas.8x8` instruction does 8 rows). From 32 rows on, the cost is one more full decode
  per 32 rows: 64 rows cost 2.0 x the 32-row time when the GPU is already full (gate+up), and nothing extra when it is
  not (down: 320 -> 640 threads).
* The last column shows the occupancy effect: `down` alone (320 threads) needs 0.247 ms = 361 G weights/s; the same matrix
  as one of four in a 1280-thread launch needs 0.119 ms (749 G weights/s).

### 4.3 Does the instruction count matter? (same kernel, fewer instructions)

| variant (gate+up, 3-bit, stage F) | instructions per 4096 weights | 1 row | 8 rows | 16 rows |
|---|---|---|---|---|
| engine code layout | 1017 | 0.291 | 0.311 | 0.326 |
| planar codes | 834 | 0.280 | 0.314 | 0.322 |
| planar codes + direct SLM offsets | 600 | 0.292 | 0.289 | 0.330 |
| planar + direct + two tables (no shift in the merge) | 738 | 0.285 | 0.289 | 0.324 |

**Removing 41 % of the instructions changes the time by at most 7 % (8 rows) and by nothing at 1 and 16 rows.** The decode
kernel is not limited by the ALU. At 0.29 ms it executes 153 G instructions/s, 40 % of the 384 G/s the 160 engines can
retire (section 7).

### 4.4 Decode once for many rows (experiment) [M]

File: `results/b60_gemm_once.csv`, source `b60/gemm_once.cpp`. A work-group of 16 threads owns 16 output rows; each thread
decodes one of the 16 column chunks of a 512-column group into SLM (8 KiB), then every thread multiplies all 16 chunks
with its own 4 / 8 / 16 / 32 input rows. The weights are decoded once per launch. Exact, checked against the host.

| rows M (gate+up, 3-bit) | engine structure (big, one decode per 32 rows) | decode once, sums in memory | decode once, sums in registers | decode once, sums in SLM |
|---|---|---|---|---|
| 64 | 0.830 | 1.015 | 0.815 | 0.948 |
| 128 | 1.450 | 1.296 | 0.930 | 1.204 |
| 256 | 2.812 | 2.046 | **1.312** | 2.554 |
| 512 | (2 x 256) | 3.847 | (2 x 1.312) | 7.312 |

For `down` (5120 x 17408, 3-bit): 256 rows take 0.659 ms with the decode-once kernel (sums in registers) against 1.306 ms
with the engine structure; 128 rows 0.472 against 0.660 ms. 4-bit codes give the same times within 5 %.

* With the sums in registers (possible up to 16 rows per thread = 256 rows per launch) one launch does 256 rows in
  1.31 ms (2.1 x faster than the engine structure): 45.6 G int8 multiply-accumulates in 1.31 ms = 70 TOPS,
  including the decode. At 64 rows the two structures are equal; the gain starts above 64 rows.
* The read-modify-write of the int64 sums in global memory after every 512-column group (what the engine's prefill
  kernel does) is expensive: 2.05 instead of 1.31 ms at 256 rows. Per input row it moves N x 16 bytes x G.
* Sums in SLM are worse: 90 KiB of SLM per work-group leaves room for only one or two work-groups per Xe-core (section 7).

## 5. Value-table variants (B60) [M]

File: `results/b60_lut_variants.csv`.

### 5.1 Inside the whole GEMM kernel (gate+up, 3-bit, 8 rows, int32 sums)

| lookup | code data | ms | G weights/s |
|---|---|---|---|
| none (state bits used as weights) | random | 0.183 | 973 |
| SLM gather, 16 KiB table (engine) | random | 0.295 | 604 |
| SLM gather, ONE 32-lane message per operand dword | random | 0.298 | 598 |
| global-memory gather, 16 KiB table | random | 0.773 | 230 |
| global-memory gather, 1 MiB pair table (4 weights per lookup) | random | 1.073 | 166 |
| none | all 16 rows of a tile share one code | 0.158 | 1130 |
| SLM gather | all 16 rows of a tile share one code (each gather hits ONE entry) | 0.214 | 834 |
| SLM gather, 32-lane messages | same | 0.200 | 893 |
| none | constant (all zero or all ones) | 0.151 | 1179 |
| SLM gather | constant (every gather hits the same entry) | 0.182 | 982 |

* The lookups cost 0.112 ms of the 0.295 ms with real (random) codes: 89 M lookups in 0.112 ms = 0.8 x 10^12 lookups/s
  of incremental capacity; the kernel as a whole does 3.0 x 10^11 lookups/s.
* **The cost of an SLM gather depends on how many different table entries its 16 lanes address**: 0.112 ms when the
  16 lanes are independent, 0.056 ms when all 16 lanes of a gather hit one (random) entry, 0.030 ms when every gather of
  the kernel hits the same entry. Real codes are the worst case.
* Packing the two gathers of an operand dword into one 32-lane message changes nothing (0.298 vs 0.295): the cost is per
  address, not per message.
* The kernel without lookups is also faster when the codes are regular (0.151 to 0.158 vs 0.183 ms): regular data are read
  faster from memory on this GPU (section 6.1). The lookup costs above are therefore differences against the lookup-free
  kernel on the SAME code data. The "no lookup" ceiling on real data is 973, not 1179 G weights/s.

### 5.2 Lookup primitive in a tight loop (4096 threads; index = in-register LCG; G lookups/s)

| primitive | random | 16 consecutive entries | one entry |
|---|---|---|---|
| no lookup (index arithmetic only) | 451 | 361 | 411 |
| SLM gather, 16 KiB | 316 | 291 | 290 |
| global gather, 16 KiB | 165 | 230 | 264 |
| global gather, 1 MiB | 44 | 131 | |
| `iselect` from a table in registers: 32 / 256 / 1024 / 2048 entries | 112 / 112 / 113 / 111 | | |

| lanes per gather message (random) | 8 | 16 | 32 |
|---|---|---|---|
| no lookup | 231 | 451 | 638 |
| SLM gather | 166 | 316 | 389 |
| global gather, 16 KiB | 116 | 165 | 175 |

* SLM gathers are 1.9 x faster than global-memory gathers into a table of the same size (L1-resident), 7 x faster than a 1 MiB
  table.
* The register table is the slowest in-cache primitive (1.1 x 10^11 lookups/s at every size): register-indirect
  addressing is slow by itself. A 4096-entry table of 16-bit values is 8 KiB, the size of the whole register file, and
  does not compile here; 2048 entries spill 11 KiB.
* Blocked / sorted indices do not help the SLM gather in this loop (the three patterns are within 9 %); the 16 addresses
  of one message here are regenerated every iteration, so this loop does not show the entry-reuse effect of 5.1.

## 6. Memory hierarchy (B60) [M]

File: `results/b60_mem.csv`. 256-byte block loads / stores unless noted; 4096 threads in the working-set sweep.
The swept buffer holds `src[i] = i`, which matters: **the read rate of this GPU depends on the content of the memory**
(section 6.1). For working sets above 16 MiB the table below is therefore optimistic for real (high-entropy) data.

| working set | seq read GB/s | seq write GB/s | copy GB/s | random gather, G accesses/s | scatter | dependent chain, 1 thread: ns per access |
|---|---|---|---|---|---|---|
| 4 KiB | 4497 | 155 | 156 | 188 | 1.2 | 67 |
| 16 KiB | 4476 | 500 | 515 | 162 | 4.5 | 68 |
| 64 KiB | 4253 | 1065 | 1002 | 157 | 16.7 | 69 |
| 256 KiB | 2964 | 1135 | 875 | 153 | 27.1 | 72 |
| 1 MiB | 1970 | 1260 | 994 | 43.5 | 26.9 | 152 |
| 4 MiB | 1984 | 1361 | 1299 | 37.9 | 27.2 | 152 |
| 16 MiB | 1997 | 1374 | 980 | 37.1 | 27.6 | 162 |
| 64 MiB | 1790 | 622 | 451 | 35.5 | 0.84 | 169 |
| 256 MiB | 719 | 380 | 144 | 2.76 | 0.54 | 600 |
| 1 GiB | 620 | | | 2.17 | | 602 |

(Small working sets written by 4096 threads at once are slow because all threads store to the same lines.)

* **Three levels are visible**: up to about 256 KiB (67 to 72 ns per dependent 16-lane gather, including the index arithmetic
  of the loop), a second level (152 to 169 ns), and DRAM (600 ns). The first step fits the "192 KB L1 / SLM per
  Xe-core" of the press material [3P]. The second level ends between 16 and 32 MiB for random data (section 6.1), which
  fits the 18 MB "L2" quoted by third parties; in this table it appears to reach 64 MiB only because the data are regular.
* **Concurrency of misses**: random gathers into 1 GiB saturate at 2.17 G line accesses/s from 320 threads on; with
  600 ns per miss that is about 1300 misses in flight [D].
* **Threads needed for the bandwidth** (1 GiB sequential read, GB/s): 1 thread 2.1, 64: 51, 160: 128, 320: 254, 640: 468,
  1280: 678, 5120: 703. A thread issues one 256-byte load per 122 ns; the bus is only filled from about 1000 threads on.
  A launch with 320 threads (down) can read at most about 250 GB/s.
* Strided 64-byte reads over 1 GiB: 5.8 G loads/s at stride 64 B, 2.3 to 3.0 G/s at 256 B to 4 KiB, 7.4 G/s at 64 KiB.

### 6.1 The read rate depends on the content of the memory

File: `results/b60_mem_data.csv`. The same 512 MiB buffer filled with different data; 4096 threads.

| working set | 1 MiB | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 MiB |
|---|---|---|---|---|---|---|---|---|---|
| seq read GB/s, random data | 1967 | 1969 | 2019 | 1915 | 930 | 805 | 632 | 415 | 381 |
| seq read GB/s, random bytes in zero-extended dwords | 1967 | 1970 | 2018 | 1915 | 936 | 814 | 737 | 646 | 572 |
| seq read GB/s, `buf[i] = i` | 1990 | 1988 | 2044 | 1998 | 1987 | 1857 | 903 | 733 | 628 |
| seq read GB/s, zeros | 2090 | 2088 | 2142 | 2095 | 2081 | 2037 | 1796 | 1689 | 1485 |
| gather G accesses/s, random data | 43.8 | 38.2 | 37.6 | 37.3 | 4.1 | 2.3 | 1.8 | 1.6 | 1.5 |
| gather G accesses/s, `buf[i] = i` | 43.6 | 38.1 | 37.5 | 37.2 | 36.8 | 35.8 | 3.9 | 2.9 | 2.4 |
| gather G accesses/s, zeros | 44.0 | 38.3 | 37.7 | 37.4 | 37.0 | 36.8 | 7.0 | 4.5 | 3.7 |

* With random data the second cache level ends between 16 and 32 MiB and the DRAM rate is 381 to 415 GB/s (the GEMM
  kernels read their random codes at 435 to 448 GB/s). With zeros or a regular pattern the same level holds four times
  as much, and 512 MiB of zeros are read at 1485 GB/s, three times what the 456 GB/s bus can carry.
  So low-entropy data do not cross the bus in full [I]. The mechanism was not identified (no public
  document was found); it behaves like transparent compression in the memory path.
* Consequences: (1) memory benchmarks on this GPU must use data as incompressible as the real data, otherwise they
  overstate cache size and bandwidth by up to 4 x; (2) the compressed weights of the engine are high-entropy by
  construction, so the engine sees the low numbers; (3) the lookup-free GEMM kernel of section 5.1 is faster on
  constant codes for this reason (its code stream is read faster), not because of anything in the compute path.

### 6.2 SLM

Work-groups of 32 threads; table of the given size per work-group:

| SLM per work-group | threads | block-load read GB/s | gather G lookups/s | ns per gather step in a thread | dependent gather: ns per hop |
|---|---|---|---|---|---|
| 4 to 64 KiB | 640 | 3750 | 159 to 170 | 60 to 64 | 56 to 60 |
| 4 to 64 KiB | 1280 | 6740 | 296 to 318 | 64 to 69 | 60 to 64 |
| 4 to 64 KiB | 4096 | 5680 | 245 to 264 | | |
| 128 KiB | 640 | 3751 | 198 | 52 | 48 |
| 128 KiB | 1280 | 3835 | 201 | 102 | 93 |

* SLM block loads reach 6.7 TB/s with 1280 threads (48 ns per 256-byte load in a thread), about the same per thread as
  L1-resident global memory (51 ns for a single thread).
* One step of the gather loop takes 64 ns in a thread against 42 ns for the same loop without the gather; a dependent
  SLM gather hop takes 56 to 60 ns (135 to 145 cycles including the index arithmetic).
* With 128 KiB per work-group the throughput stops growing at 640 threads: not all work-groups fit at once (section 7).

## 7. Arithmetic, registers, threads, launches (B60) [M]

### 7.1 Arithmetic

Files: `results/b60_arith.csv`, loop bodies in `results/b60_asm_loop_bodies.txt`. "Peak" = 8 independent chains, 5120
threads (the GPU is full). Lane-operations count every instruction of a two-instruction step (`mul+add`, `add+xor`).

| operation | ESIMD `simd<T,16>`: peak, 10^12 lane-ops/s | plain SYCL (compiled SIMD32): peak | ESIMD dependent chain, 160 threads: ns per step |
|---|---|---|---|
| f32 add | 6.11 | | 6.3 |
| f32 mul | 6.11 | | 6.3 |
| f32 mul+add (unfused) | 6.13 | 11.6 | 10.5 |
| f16 mul+add | 6.13 | 11.6 | 10.5 |
| f64 add / mul | 0.38 / 0.38 | | 28.5 |
| f64 mul+add | 0.38 | 1.46 | 55.2 |
| f32 `operator/` (`math.inv` + `mul`, not IEEE) | 1.38 (per divide) | | 17.9 |
| f32 `div_ieee` | 0.26 | 0.52 (`/`, IEEE by flag) | 79 |
| f32 `sqrt` | 1.39 | | 13.6 |
| f32 `sqrt_ieee` | 0.23 | 0.53 | 91 |
| i32 add+xor | 5.85 | 11.5 | 10.5 |
| i32 mul+xor (`mul`+`macl`+`xor`) | 1.98 (per step) | 3.67 | 13.1 |
| i64 add+xor | 2.84 | 7.8 | 12.0 |
| i64 mul+xor (11-instruction multiply) | 0.30 (per step) | 1.08 | 44.4 |

* **One engine retires one 16-lane instruction per clock when it is full**: 6.11 x 10^12 lane-ops/s = 160 x 16 x 2.39 GHz.
  That is 384 G instructions/s for the card.
* **A 32-lane instruction costs the same as a 16-lane one**: the plain SYCL kernels, which IGC compiles to SIMD32, reach
  11.5 to 11.6 x 10^12 lane-ops/s, twice the ESIMD kernels that are written with 16-lane vectors. The same shows inside
  ESIMD: the 32-lane index loop of section 5.2 does 638 against 451 G lanes/s.
* **Dependent instructions are slow**: one more dependent add or multiply in a chain costs 4.2 ns = 10 cycles
  ((10.45 - 6.28) ns per extra dependent operation; float and int32 alike), and a loop trip never takes less than about
  21 ns = 50 cycles even with a single add in it (`reg,add32` rows of `results/b60_xmx.csv`). A single thread therefore
  executes a short dependent loop at a few percent of the engine's rate; the engine is only filled by its 8 threads
  running interleaved, or by independent work inside the thread.
* FP64 exists and is 16 x slower per lane than FP32. IEEE divide and square root are 24 to 27 x slower than an add.
* The exact accumulation `acc64 += sext(d32) * (int64)A32` runs at 0.58 x 10^12 lane-steps/s against 1.27 for the
  wrapping int32 form and 0.77 for "32-bit multiply, 64-bit add"; doing it in double precision is 5 x slower (0.12).

### 7.2 Registers

File: `results/b60_regs.csv` (`live`), spill sizes in `results/b60_igc_asm_counts.txt`.

| live `simd<uint32,16>` values (64 bytes each) | 8 to 124 | 128 | 144 | 160 | 192 | 256 |
|---|---|---|---|---|---|---|
| ns per register update (160 threads) | 3.4 to 4.2 | 4.9 | 7.3 | 8.8 | 14.1 | 34.4 |
| spilled bytes (assembly) | 0 | 832 | 1856 | 2880 | 4992 | 9088 |

Up to 124 live 64-byte values there is no spill; at 128 the allocator spills 13 of them. Of the 128 registers about 4
are reserved (r0, the thread payload, stack and scratch pointers).

### 7.3 Threads, work-groups, SLM

| threads (ALU-bound kernel, 16 live values) | 64 | 160 | 320 | 480 | 640 | 960 | 1280 | 2560 | 5120 |
|---|---|---|---|---|---|---|---|---|---|
| time for the same work per thread, ms | 0.81 | 0.88 | 0.92 | 1.01 | 1.22 | 1.81 | 2.42 | 4.82 | 9.63 |
| G register updates/s | 25 | 58 | 111 | 152 | 167 | 169 | 170 | 170 | 170 |

| threads (latency-bound loop: one add per trip) | 160 | 640 | 960 | 1280 | 1920 | 2560 | 5120 |
|---|---|---|---|---|---|---|---|
| ns per trip in a thread | 22.8 | 22.8 | 25.2 | 25.2 | 50.1 | 50.3 | 100.3 |
| G trips/s | 7.0 | 28.0 | 38.2 | 50.9 | 38.3 | 50.9 | 51.1 |

* **1280 threads run at the same time** (8 per engine), not 640: a latency-bound loop keeps its per-thread speed up to
  1280 threads and the time doubles exactly at 2560. An ALU-bound kernel already fills the engines with 4 threads each,
  so its throughput stops growing at about 640 threads; that is the "640" seen earlier.
* Work-group size (1 to 64 threads) makes no difference for a kernel without SLM; an ESIMD work-group cannot have more
  than 64 threads ("Total number of work-items in a work-group cannot exceed 64 for this kernel").
* SLM limits how many work-groups run at once. Throughput of the ALU kernel at 1280+ threads, G updates/s:

| SLM per work-group | 0 to 16 KiB | 32 KiB | 64 KiB | 128 KiB |
|---|---|---|---|---|
| work-groups of 32 threads | 170 to 175 | 175 | 175 | 172 |
| work-groups of 8 threads | 170 to 175 | 148 to 163 | 112 | 58 |

  With 8 threads and 128 KiB per work-group only a third of the engines are busy: about one such work-group fits per
  Xe-core at a time. A kernel should need at most about 2 KiB of SLM per thread of its work-group to keep the GPU full.

### 7.4 Launch

| | microseconds |
|---|---|
| empty kernel, submit + wait | 5.8 (plain SYCL), 6.8 (ESIMD) |
| empty kernel, queued (one wait per batch) | 2.8, 3.0 |
| smallest real kernel seen (1280 threads, 10 MiB int8 GEMM), queued / sync | 5.1 / 11.4 |
| 64-byte read-back to the host + wait | 8.7 |
| `queue.wait()` on an idle queue | 0.25 |

In the GEMM itself the per-launch cost shows as the difference between short and long threads: gate+up (10 column groups
per thread) decodes 614 G weights/s, the 34-group launch 749 G weights/s. `ms_sync` is 7 to 8 us above `ms_queued` in every
row of every file.

## 8. XMX (B60) [M]

File: `results/b60_xmx.csv`.

### 8.1 `dpas` in registers

One "row" = one input row of one `dpas` = 16 lanes x depth 8 x 4 int8 products = 512 multiply-accumulates (bf16: 256).

| | 1 thread | 640 threads | 1280 threads | 5120 threads |
|---|---|---|---|---|
| `dpas.8x1`, dependent chain: ns per dpas in a thread | 21.8 | 23.2 | 45.1 | 146 |
| `dpas.8x1`, 4 independent accumulators: ns per dpas | 6.0 | 6.3 | 6.7 | 26.8 |
| `dpas.8x8`, dependent chain: ns per ROW | 2.75 | 3.38 | 8.7 | 32 |
| card total, 4 x `dpas.8x1`, G rows/s | 0.2 | 101 | 190 | 191 |
| card total, `dpas.8x8` chain, G rows/s | 0.4 | 189 | 147 | 159 |
| int8 TOPS (4 x `dpas.8x1`) | | 103 | 195 | 196 |
| bf16 TFLOPS (same loops with bf16 operands) | | 51 | 98 | 98 |

* **Peak: 191 G dpas rows/s = 196 TOPS int8 = 98 TFLOPS bf16.** Per engine that is one 16-lane row every 2.0 cycles =
  256 int8 multiply-accumulates per clock, exactly the "INT8 4096 ops/clock per Xe-core" of Intel's slides [3P].
* `dpas.8x8` (8 rows in one instruction) reaches the same peak with a quarter of the threads: 189 G rows/s at 640 threads.
* Latency: a loop trip with one dependent `dpas.8x1` takes as long as a trip with one dependent add (21.8 vs 21.7 ns), so
  the latency of a `dpas` is hidden under the 50-cycle loop floor. When 8 threads share an engine, the trip takes 13.5
  cycles of engine time against 7.5 for the add loop: a dependent `dpas.8x1` occupies the engine for about 7 cycles,
  an independent one for about 2. bf16 and int8 behave the same.

### 8.2 Uncompressed GEMM (pre-decoded int8 weights)

| N x K | rows M | rows per thread | ms | TOPS | weight GB/s |
|---|---|---|---|---|---|
| 17408 x 5120 (89 MB of weights) | 1 | 1 | 0.207 | 0.86 | 430 |
| | 8 | 8 | 0.212 | 6.7 | 421 |
| | 16 | 16 | 0.216 | 13.2 | 414 |
| | 64 | 64 (8 x `dpas.8x8`) | 0.230 | 49.6 | 388 |
| | 64 | 32 (weights read twice) | 0.404 | 28.3 | 221 |
| | 256 | 32 (weights read 8 times) | 1.536 | 29.7 | 58 |
| | 1024 | 32 (weights read 32 times) | 5.988 | 30.5 | 15 |
| | 1024 | 16, weights staged once in SLM per 16 output rows | 2.225 | **82.0** | 40 |
| 20480 x 512 (10 MB, cache resident) | 8 | 8 | 0.0056 | 30 | |
| | 64 | 64 | 0.0120 | **111** | |
| 81920 x 512 (42 MB) | 64 | 64 | 0.165 | 32.6 | 255 |

* With weights in global memory the product is limited by the bus, not by XMX: 430 GB/s at 1 row, still 388 GB/s with 64
  rows per thread. Every additional pass over the weights costs a full 0.2 ms.
* XMX itself delivers 111 TOPS on cache-resident weights and 82 TOPS for 1024 rows when a work-group stages its weight
  tile in SLM once; the remaining factor to the 196 TOPS peak is operand traffic (an SLM block load of the weights and a
  global block load of the input rows for every `dpas.8x8`).
* bf16 runs at half the rate per byte (2 instead of 4 products per dword): 25.6 TFLOPS at 64 rows per thread.

## 9. Generated code (static analysis; nothing in this section needed a GPU)

### 9.1 B60: what IGC makes of the GEMM kernel

Source: `results/b60_igc_asm_counts.txt` (all kernels of `gemm_stages`), made with `scripts/igc_asm_count.py` from an
`IGC_ShaderDumpEnable=1` dump. The loop body is one basic block that handles one 256-column half block of 16 weight rows =
**4096 weights** (2048 table lookups). `k_small<6, 8, F>` is the engine's `gemm<6, 8>` kernel line for line.

Instructions in the loop body per 4096 weights [M: counted in the assembly]:

| kernel | total | SLM gathers (16 lanes) | shifts | and / or | mov | dpas | USM block loads | SLM block loads | spill + fill |
|---|---|---|---|---|---|---|---|---|---|
| engine layout, 3-bit, 1 row | 1018 | 128 | 428 | 241 | 162 | 8 (`dpas.8x1`) | 14 | 0 | 0 |
| engine layout, 3-bit, 8 rows | 1017 | 128 | 428 | 241 | 162 | 8 (`dpas.8x8`) | 14 | 0 | 0 |
| engine layout, 3-bit, 16 rows | 1033 | 128 | 428 | 241 | 162 | 16 | 22 | 0 | 0 |
| engine layout, 4-bit, 8 rows | 939 | 128 | 423 | 231 | 98 | 8 | 16 | 0 | 0 |
| planar codes, 3-bit, 8 rows | 834 | 128 | 269 | 233 | 144 | 8 | 14 | 0 | 0 |
| planar codes + direct offsets, 3-bit, 8 rows | **600** | 128 | 230 | 155 | 27 | 8 | 14 | 0 | 0 |
| planar codes + direct offsets, 4-bit, 8 rows | **594** | 128 | 233 | 169 | 3 | 8 | 16 | 0 | 0 |
| planar + direct + two tables, 3-bit, 8 rows | 738 | 128 | 166 | 123 | 195 | 8 | 14 | 0 | 0 |
| big, 16 rows per thread (TR = 2), 3-bit | 1166 | 128 | 428 | 257 | 162 | 128 (`dpas.8x1`) | 6 | 16 | 0 |
| big, 32 rows per thread (TR = 4), 3-bit (engine prefill) | 1350 | 128 | 428 | 273 | 162 | 256 | 6 | 32 | 0 |
| big, 64 rows per thread (TR = 8), 3-bit | 2427 | 128 | 429 | 241 | 303 | 512 | 6 | 64 | **211 + 181** |

What the listing shows [M]:

1. **One `dpas` instruction does all 8 rows.** `xmx::dpas<8, 8, ...>` becomes a single `dpas.8x8 (16|M0)` (16 lanes, depth 8,
   repeat count 8 = 4096 multiply-accumulates). So rows cost no extra instructions up to 8 per thread; this is why
   1, 8 and 16 rows take almost the same time. The big kernel uses 8 separate `dpas.8x1` per tile instead
   (256 instructions for 32 rows where 4 `dpas.8x8` per 32-column chunk = 32 instructions would do the same arithmetic).
2. **The decode is 2/3 shifts and masks**, and two source-level details double their number:
   * *Interleaved code words.* The stream stores the 4 dwords of a row together (`[lane][4 dwords]`), so each code word
     is the stride-4 selection `blk.select<16, 4>(k)` of a 4-register block. A region with stride 4 spans more than two
     registers, so IGC splits every such shift into two 8-lane instructions (`shr (8|M0) r73.0 <- r7.0<4;1,0>` and
     `shr (8|M0) r73.8 <- r9.0<4;1,0>`): 370 of the 1017 instructions are 8-lane halves. Storing the codes "planar"
     (`[4 dwords][16 lanes]`, a load-time re-layout of the same bytes) removes them: 1017 -> 834.
   * *State, then x4.* `((w >> s) | (w1 << (32 - s))) & 0xfff` followed by `* 4` makes LLVM narrow the result to 16 bit:
     IGC emits `shl :w`, `mov :w`, `mov :hf`, `and :w`, `mov :d <- :uw` (5 instructions) after the funnel shift for every
     state that straddles two words (a third of them for 6-bit steps). Extracting the SLM byte offset directly
     (`... >> (s - 2)) & 0x3ffc`, `offset_of` in `gemm_stages.cpp`) removes them: 834 -> 600 (4-bit: 594).
     The simple (non-straddling) states were already 2 instructions (`shr`, `and 0x3FFC`).
3. **No spills in the engine's kernels** up to 32 rows per thread. The 64-rows-per-thread kernel spills 4 KiB and has 392 spill /
   fill sends in the loop body.
4. **The int64 accumulation is cheap in instructions**: `acc += sext(d) * (int64)Aw` is `mul` + `mach` (32 x 32 -> 64 in two
   16-lane instructions), three `mov` to interleave the halves, and one native 64-bit `add` (16 qwords in one instruction):
   about 6 instructions per row per 512 columns, i.e. 3 per loop body per row, against ~1000 for the decode.
   A general 64 x 64 multiply is 11 instructions (`mul`/`mach`, 2 x `mul`/`macl`, `add3`, 4 `mov`): Xe2 has no 64-bit
   integer multiplier in the vector engine, IGC builds it from 32 x 16 and 32 x 32 steps.
5. A 32-bit multiply by a 32-bit value is 2 instructions (`mul` into the accumulator register + `macl`).
6. `operator/` on `simd<float>` is `math.inv` + `mul` (not correctly rounded), `div_ieee` is `math.invm` + a correction
   sequence, `sqrt_ieee` is `math.rsqtm` + correction; `sqrt` is one `math.sqt`.
7. `simd::iselect` is true register-indirect addressing (`add a0.0, idx, base` then `r[a0.0]<1,0>:uw` as a source), no
   memory access, as long as the table fits in registers. The register file is 128 x 64 B = 8 KiB, so: 256 entries of
   16 bit (8 registers) and 1024 entries (32 registers) compile without or with small spills (0 / 1.2 KiB), 2048 entries spill 11 KiB, and
   **4096 x 16 bit = 8 KiB = the entire register file cannot be register-resident** (the kernel does not compile: "V566 (size = 8192 bytes)
   is indirectly addressed and spans complete GRF file ... This cannot be allocated by RA", `results/b60_isel4096_compile_error.txt`).

### 9.2 RTX 5060 Ti: what nvcc makes of the upstream kernels

Source: `results/cuda_sass_counts.txt` (SASS for sm_120 from `nvcc -O3 --fmad=false`, counted with `scripts/sass_count.py`);
listings in `results/cuda_codegen/` (regenerate them with `scripts/cuda_codegen.sh`; they are not kept in the repository). A SASS listing is the program of one thread; every instruction is executed by the 32
lanes of a warp. One loop trip of the decode kernels handles the same unit as on the B60: 16 rows x 256 columns = 4096 weights.

Which kernel runs by default (from the upstream source, `engine35.cpp:403-420`): 1..16 rows -> `lnord_kernel<6, 8|16, XSUM>`
(weights are re-laid out once at load into its own order), 17..255 rows -> `pfa_kernel`, 256 rows and more -> the weights
are **decoded once to int8** (`mt_predecode2_kernel`) and multiplied by an ordinary tensor-core GEMM (`mt_gemm6_kernel`).
`gemm_kernel` of `gemm_compressed.cu` is not used by the current engine.

Per loop trip (4096 weights), per thread [counted in SASS]:

| | LNORD 1, 3 rows | LNORD 2, 4..8 rows | LNORD 9..16 rows | PFA (17..255 rows) | B60 engine kernel, 8 rows | B60 planar + direct |
|---|---|---|---|---|---|---|
| input rows handled per trip | 8 | 8 | 16 | 64 | 8 | 8 |
| tensor-core / dpas instructions | 8 `IMMA.16832.S8.S8` | 8 | 16 | 64 | 8 `dpas.8x8` | 8 |
| table lookups | 64 `LDS.U16` | 64 | 64 | 64 | 128 `load.slm.d32` | 128 |
| shifts + logic (+ multiply-as-shift) | 199 | 209 | 199 | 218 | 669 | 385 |
| moves | 32 | 31 | 44 | 261 | 162 | 27 |
| spill fills | 0 | 0 | 0 | 0 | 0 | 0 |
| all instructions | 357 | 372 | 396 | 828 | 1017 | 600 |
| lanes per instruction | 32 | 32 | 32 | 32 | 16 (370 of them 8) | 16 |
| **lane-operations per weight** | **2.79** | **2.91** | **3.09** | 6.46 | **3.03** | **2.11** |
| registers per thread | 80 | 64 | 128 | 222 | 128 GRF x 64 B | |

(lane-operations per weight = instructions x lanes / 4096; for the B60 engine kernel (588 x 16 + 370 x 8 + 38) / 4096, for the
planar + direct kernel (539 x 16 + 37) / 4096.)

Reading [D from the two counts]:

* Both compilers do the same work per weight to within about 10 %: one table lookup per two weights on both sides, 1.6
  (CUDA) vs 1.9 (B60 engine kernel) shift / mask lane-operations per weight, one matrix instruction per 4096
  multiply-accumulates on both sides. **Code generation is not where a factor of 2 comes from.** With the planar + direct
  variant the B60 loop needs fewer lane-operations per weight than the CUDA loop.
* The 64-bit accumulation on CUDA is one instruction, `IMAD.WIDE` (signed 32 x 32 -> 64 multiply-add); on the B60 it is 6.
  It runs once per 512 columns on both, so it is under 1 % of either loop.
* The CUDA table is 4096 x uint16 (8 KiB) read with scalar `LDS.U16` (one address per lane, as every load in SIMT);
  the B60 reads it with a 16-address gather message to the data port.
* PFA spends 261 moves per trip to place `mma` fragments in consecutive registers and uses 222 registers per thread; it
  handles 64 rows per decode, 0.10 lane-operations per multiply-accumulate. The G6 GEMM (pre-decoded weights) needs only
  0.02 to 0.03 lane-operations per multiply-accumulate, because no decode is left in its loop.
* No float `fma` is generated anywhere in the hot loops (unfused, as the engine requires).

## 10. The ten candidate causes

Engine-level numbers quoted here ("B60 pass 61 ms at 8 rows", "5060 Ti 35 ms per pass", tokens per second) are from the port
notes of the engine, measured before this study; they are not re-measured here. One pass decodes 25.6 G weights = 9.6 GB
of 3-bit codes.

| # | candidate | what the measurements show (B60) | 5060 Ti | verdict |
|---|---|---|---|---|
| 1 | raw compute | [M] 6.1 x 10^12 lane-ops/s with 16-lane ESIMD instructions (one instruction per engine per clock), 11.6 x 10^12 with 32-lane instructions. [M] The decode kernel uses 40 % of that, and removing 41 % of its instructions does not make it faster. | not measured. [D] 4608 lanes x 2.4 to 2.6 GHz = 11 to 12 x 10^12 if every lane retires one operation per clock [I]. | Not what limits the GEMM. Matters for the small non-GEMM kernels (see 6). |
| 2 | XMX vs tensor cores | [M] peak 196 TOPS int8 / 98 TFLOPS bf16; the decode kernel needs 5 % of it at 8 rows (dpas = 7 % of the kernel time). [M] One `dpas.8x8` instruction = 4096 multiply-accumulates, the same as one `mma.sync m16n8k32` [O]. | not measured; [O] "759 AI TOPS" (marketing unit). | Not a cause for decoding. For prompt reading the rate reached depends on how often the weights are decoded and re-read, not on XMX (82 to 111 TOPS reachable). |
| 3 | memory hierarchy | [M] 435 to 448 GB/s on the codes, close to the card's 456 GB/s [O]; the read alone is 53 % of the decode kernel. [M] Needs about 1000 threads to fill the bus. [M] Gathers: SLM 3.2 x 10^11/s in a loop, 1.9 x faster than L1-resident global memory. | not measured; 448 GB/s [3P]. | The bus sets the floor (22 ms per pass on the B60, 21 ms on the 5060 Ti [D]). The B60 GEMM sits at 1.6 to 1.9 x its floor. Under-filled launches (down: 320 threads) are bus-starved. |
| 4 | decompression speed | [M] 614 (gate+up) to 749 (long threads) G weights/s = 230 to 281 GB/s of codes. Split: read 53 %, states 11 %, lookups 28 %, dpas 7 %, int64 1 %. [M] The lookups cost 0.11 ms per 178 M weights and the cost depends on the number of distinct entries per gather; no table variant is faster than SLM. | not measured. [M: SASS] the CUDA kernel does the same number of lookups per weight (one `LDS.U16` per two weights) and 2.8 to 3.1 lane-operations per weight (B60: 3.0). | **The main cause on the GEMM side**: on the B60 the table lookups and the code stream do not overlap in time (the stages add up: 0.15 read + 0.03 + 0.11 lookups = 0.29 ms). Whether the 5060 Ti overlaps them could not be measured. |
| 5 | 64-bit integer cost | [M] stage F - stage E = 0.003 to 0.02 ms (1 to 6 %) up to 16 rows. [M] 6 instructions per row per 512 columns (`mul`, `mach`, 3 `mov`, 64-bit `add`). [M] In the prefill structure the cost is the read-modify-write of the sums in memory, not the multiply: 2.05 vs 1.31 ms at 256 rows. | [M: SASS] one `IMAD.WIDE` per sum. | Not a cause for decoding. A cause for prompt reading through the memory traffic of the sums. |
| 6 | thread / SIMD scheduling | [M] 1280 threads resident; a thread executes a dependent operation every 10 cycles and a loop trip takes at least 50 cycles, so the engine is only full with 8 interleaved threads or independent work in a thread. [M] gate+up (10 groups per thread) 614 vs 749 G weights/s for long threads; down with 320 threads 361 G weights/s. [M] 16-lane ESIMD instructions use half of what a 32-lane instruction does per clock. | not measured. [O] 48 warps per SM resident; a warp is 32 lanes. | **A cause**: launches with fewer than about 1280 threads and kernels with short dependent loops run far below the card's rate. This is where most of the non-GEMM time and part of the GEMM time go [I for the non-GEMM share]. |
| 7 | registers / occupancy | [M] 124 live 64-byte registers before the first spill; none of the engine's kernels up to 32 rows per thread spills; 64 rows per thread spills 4 KiB (392 spill / fill sends per 4096 weights) and is 44 % slower. [M] SLM above about 2 KiB per thread of a work-group reduces the number of work-groups that run. | [M: SASS, compile time] 64 to 128 registers per thread in the decode kernels, 222 in PFA, 254 in the G6 GEMM; no spill in any 3-bit hot loop. | Not a cause in the current kernels. It is the constraint that forces "32 rows per decode" in the prefill kernel (see 10). |
| 8 | kernel launch and sync | [M] 2.8 to 3.0 us per queued launch, 5.8 to 6.8 us with a wait, 8.7 us for a small read-back; `ms_sync - ms_queued` = 7 to 8 us everywhere. About 830 launches per pass [from the port notes: 13 per layer] = 2.4 ms queued, 5.6 ms if each is waited for [D]. | not measured. | A small cause: 4 to 9 % of a 61 ms pass. Larger in effect through 6 (each short launch also under-fills the GPU). |
| 9 | compiler code generation | [M] IGC: 1017 instructions per 4096 weights, 3.0 lane-ops per weight; nvcc: 357 to 396 warp instructions, 2.8 to 3.1 lane-ops per weight. Two avoidable patterns (stride-4 code words, state x 4) inflate the IGC code by 70 %, but removing them does not change the run time. No spills, no float contraction on either side. | [M: SASS] | Not a cause. |
| 10 | algorithm / implementation | [source] CUDA: decode kernels with a column-group worker split and one-trip-ahead prefetch for 1 to 16 rows; 64 rows per decode for 17 to 255 rows (PFA); **from 256 rows on the weights are decoded once to int8 and multiplied by a plain tensor-core GEMM** (G6), prefill chunks are 1024 rows. B60 engine: one decode per 32 rows at any batch size, sums read and written in memory per 512 columns. [M] On the B60 a decode-once kernel does 256 rows in 1.31 ms where the engine's structure needs 2.81 ms. | [source + SASS] G6: 0.02 to 0.03 lane-operations per multiply-accumulate. | **The main cause for prompt reading.** |

### How the gap divides

Decoding (per pass, 8 to 16 rows; [D] from the port notes' pass times and the rates measured here):

* Per pass [D from the port notes: tokens per second / tokens per pass]: B60 74.6 ms, 5060 Ti 36.2 ms, a deficit of 38 ms.
* GEMM on the B60: 25.6 G weights at 570 to 750 G weights/s (in-engine rate of the port notes to the best rate measured
  here) = 34 to 45 ms. The bus floor is 22 ms on the B60 and 21 ms on the 5060 Ti. So the B60 spends **12 to 23 ms of GEMM
  time above the floor** (lookups 28 %, state extraction 11 %, under-filled and short launches up to 20 %), and
  30 to 41 ms outside the GEMM (non-GEMM kernels, about 830 launches, head, state replay, drafter).
* How the 5060 Ti's 36 ms divide is not known (not measured). Its GEMM cannot take less than 21 ms, so everything else
  takes at most 15 ms there. Hence **at most 13 to 24 ms of the 38 ms deficit is GEMM and at least 15 ms is outside the
  GEMM**; if the 5060 Ti's GEMM is not at its floor, the non-GEMM share is larger still.
* This study did not profile the non-GEMM kernels. What it shows is why short kernels are slow on this GPU: 10 cycles per
  dependent operation, 50 cycles per loop trip, 3 to 7 us per launch, half the lane rate with 16-lane vectors, and a
  launch below about 1280 threads leaves engines and bus idle.

Prompt reading: with batches of 128 to 256 rows the B60 kernel decodes every weight 4 to 8 times per batch and moves
N x 16 bytes x G of sums per row; the CUDA engine decodes once per 1024 rows. Measured on the B60, a kernel that decodes
once per 256 rows and keeps the sums in registers is 2.1 x faster for gate+up (2.0 x for down) than the present structure.
Attention and the other prefill kernels were not examined here.

## 11. What matters most for making the engine faster on the B60

1. **Prefill GEMM: decode once per launch, sums in registers** (`b60/gemm_once.cpp`, exact). Work-group = 16 threads on
   one 16-row tile; each thread decodes one 32-column chunk into SLM (24.5 KiB per work-group in total, which keeps the GPU
   full), then multiplies all chunks with its own 16 rows using two `dpas.8x8` per chunk; int64 sums stay in registers
   (2 per row) and are stored once. 256 rows per launch in 1.31 ms for gate+up against 2.81 ms now.
   Needs the codes in a word-planar layout (`[half block][word][32 lanes]`), a load-time permutation.
   Do not put the sums in SLM (7.3 ms at 512 rows) and do not exceed 16 rows per thread with this layout.
2. **Fill the GPU: 1280 threads, long threads.** The same kernel decodes 749 G weights/s with 1280 long threads, 614 in the
   gate+up launch (2176 threads x 10 groups) and 361 with 320 threads. `down` alone should be split over 4 column workers
   (1280 threads), not 2; matrices with few column groups per thread should share launches so that the per-thread start-up
   is paid less often. Expected: up to about 20 % of the GEMM time [D from 614 vs 749].
3. **The decode kernel itself is at its practical limit for this weight format**: 0.15 ms of bus time + 0.11 ms of
   lookups + 0.03 ms. Fewer instructions (planar codes, direct offsets: -41 %), 32-lane gather messages, a second table,
   global-memory or register tables do not help or are slower. The only lever found on the lookups is their address
   diversity (a gather whose 16 lanes hit one entry costs half), which real codes do not offer.
4. **Non-GEMM kernels**: write the ALU parts with 32-lane vectors (`simd<T, 32>`, two registers; same cost per instruction
   as 16 lanes), keep several independent streams per thread instead of one dependent chain, avoid short run-time loops
   (50 cycles per trip), avoid IEEE divide / sqrt where a multiply by a reciprocal computed once is exact enough, and merge
   launches (3 us each, plus an under-filled GPU while they run).
5. Keep SLM at or below about 2 KiB per thread of the work-group, and at most 124 live 64-byte values per thread.

## 12. Differences from the earlier notes

| earlier statement | this study |
|---|---|
| "the GPU runs about 640 ESIMD threads at once" | 1280 threads are resident and run (8 per engine, as the driver source says). A latency-bound loop scales to 1280 threads; an ALU-bound kernel saturates at about 640 because 4 threads already fill an engine. Memory-bound kernels (code reading) still gain from 640 to 1280 threads (468 -> 678 GB/s). |
| "one dpas (depth 8) takes about 10 EU cycles" | Throughput is one 16-lane row per 2 cycles per engine (191 G rows/s, 196 TOPS). About 7 cycles of engine time when each `dpas` depends on the previous one. `dpas<8, 8>` is one instruction for 8 rows. |
| "register-resident table with `iselect`: 1.1 to 1.3 x 10^11 lookups/s" (4096 entries) | The rate is confirmed (1.1 x 10^11) but it is the same for 32 to 2048 entries, i.e. it is the cost of register-indirect addressing. A 4096 x 16-bit table is 8 KiB = the whole register file and cannot be register-resident; here the register allocator rejects it (`results/b60_isel4096_compile_error.txt`), and 2048 entries already spill 11 KiB. The earlier 4096-entry number was most likely measured on a spilled table [I]. |
| "a kernel without table lookups reached 1226 to 1323 G weights/s" | 973 G weights/s for gate+up with random codes; 1130 to 1179 only when the 16 rows of a tile carry identical codes. Reading the codes alone is 1161 to 1195. |
| "SLM lookups about 600, USM gathers about 300, 18-bit table 196" | reproduced: 604, 230, 166. |
| "rows are nearly free up to 16", "64 rows: 0.82 / 1.16 / 1.22 ms with 32 / 64 / 16 rows per thread" | reproduced: 0.29 / 0.31 / 0.33 ms for 1 / 8 / 16 rows; 0.83 / 1.20 / 1.24 ms. The 64-rows-per-thread kernel is slow because it spills 4 KiB. |
| "USM block loads of the input rows cost about as much as the dpas products" | Both together are 7 % of the decode kernel (stage E - stage D = 0.02 ms). |
| "`SYCL_CACHE_PERSISTENT=1` avoids recompiling" | The Level Zero driver has its own on-disk kernel cache as well; an `IGC_ShaderDumpEnable` dump stays empty for cached kernels unless `NEO_CACHE_PERSISTENT=0` is set. |
| memory bandwidth "456 GB/s" | 381 to 448 GB/s measured on random data (435 to 448 in the GEMM kernels). Regular or zero data are read up to 4 x faster than the bus rate (section 6.1), so a benchmark with synthetic regular data overstates the bandwidth. |

Also observed while waiting for the GPU [M, `/proc/<pid>/fdinfo`]: with two engine processes on the B60 at once, one held
22.0 GB of VRAM and the other had 3.0 GB in VRAM and 13.5 GB of its buffers in system memory. One of them, a profiling run of a few passes, was still
running after 40 minutes. Two engine instances do not fit the 24 GB card together.

## 13. Not measured, and why

* **Every RTX 5060 Ti run-time number.** A compute process (15.8 GB, 100 % utilisation) used the card for the whole
  session, so no CUDA benchmark was executed. The programs in `cuda/` compile (`scripts/build_cuda.sh`) and
  `scripts/run_cuda.sh` will produce `results/5060ti_*.csv` when the card is free. Until then the comparison rests on:
  the engine-level numbers of the port notes, the vendor / third-party facts of section 2, and the SASS of the upstream
  kernels. In particular the following are unknown: the 5060 Ti's GEMM rate by stage, its lookup rate from shared
  memory, tensor-core latency / throughput, cache steps, launch cost, occupancy.
* The CUDA mirrors use a synthetic code layout and no `cp.async` ring; they measure the same kind of work per weight, not
  the upstream kernel itself. Their bf16 `mma` section has no host check.
* The large register mode (256 GRF) on the B60: excluded on purpose (it hung the GPU before).
* The engine's own non-GEMM kernels, attention and drafter were not profiled; the statements about them in section 10
  are inferences from the micro-benchmarks.
* Stage C of the GEMM decomposition is not a valid intermediate (extra spills); stage G of the "big" structure contains
  spills of its own (its float conversion is not how the engine does it).
* The mechanism behind the content-dependent read rate (section 6.1) was not identified.
* The latency columns of section 6 (dependent chains) were measured on the index-pattern buffer only; for random data
  the second level is smaller (section 6.1) but its latency was not re-measured.
* All cache and latency numbers come from 16-lane ESIMD loads / gathers; scalar loads were not measured separately.
