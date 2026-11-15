# Architecture micro-benchmarks: Intel Arc Pro B60 vs NVIDIA RTX 5060 Ti

Micro-benchmarks that take the compressed GEMM of the engine apart, to explain by measurement why the same engine is
slower on the Arc Pro B60 (Xe2, SYCL ESIMD + Level Zero) than on the RTX 5060 Ti (CUDA), although both cards have about the
same memory bandwidth. The analysis is in [REPORT.md](REPORT.md); raw numbers are in `results/`.

## Layout

| path | content |
|---|---|
| `b60/common.hpp` | queue selection (Intel GPU), `unroll<N>`, timing (`best_time`: best of 5, queued or synchronous), `ARCH_ONLY`, `ARCH_COMPILE_ONLY` |
| `b60/gemm_stages.cpp` | stage decomposition A..G of the compressed GEMM (read codes, extract states, table lookup, int8 operand, dpas, int64 accumulation, float scale); 3- and 4-bit codes, M = 1..64 rows, real matrix shapes; exact check against a host reference |
| `b60/gemm_decode.cpp` | experiment: the decode GEMM (1 to 16 input rows) with code words and lookups requested ahead, and with 2 to 8 workers per output block; results in `results/b60_gemm_decode*.csv` (no variant was faster in the engine) |
| `b60/gemm_once.cpp` | experiment: compressed GEMM that decodes the weights once per launch for 64 to 512 input rows (work-group of 16 threads per 16 output rows, decoded operands in SLM, int64 sums in registers / memory / SLM); exact |
| `b60/lut_variants.cpp` | value-table variants: SLM gather, USM gather, 1 MiB pair table, register table (`iselect`), index patterns, 8 / 16 / 32 lanes per gather message; in a tight loop and inside the whole GEMM kernel |
| `b60/xmx.cpp` | `dpas` latency / throughput in registers (int8 and bf16) swept over the number of threads; uncompressed int8 / bf16 GEMM |
| `b60/mem.cpp` | sequential read / write / copy, strided read, gather / scatter, pointer chasing, SLM, for working sets from 4 KiB to 1 GiB; read rate for different memory contents |
| `b60/arith.cpp` | FP32 / FP16 / FP64 / INT32 / INT64 latency and throughput (ESIMD and plain SYCL), the int64 accumulation forms |
| `b60/regs.cpp` | live registers (spill threshold), work-group size, number of threads, SLM per work-group, launch overhead |
| `cuda/*.cu` | CUDA versions of the same benchmarks (same operation, same data sizes) |
| `scripts/build_b60.sh`, `scripts/build_cuda.sh` | build into `bin/` |
| `scripts/b60run.sh` | runs one command on the B60 under a lock file, after waiting for other users of the GPU |
| `scripts/run_b60.sh`, `scripts/run_cuda.sh` | run everything in short segments, write `results/b60_*.csv` / `results/5060ti_*.csv` |
| `scripts/igc_asm_count.py`, `scripts/igc_loop_body.py` | instruction counts and loop bodies from IGC (Xe) assembly dumps |
| `scripts/cuda_codegen.sh`, `scripts/sass_count.py` | PTX / SASS of the upstream CUDA GEMM kernels and instruction counts |
| `results/` | raw results: `b60_*.csv` (measured on the B60), `b60_igc_asm_counts.txt` and `b60_asm_loop_bodies.txt` (IGC assembly), `cuda_sass_counts.txt` and `cuda_codegen/` (SASS / PTX of the upstream CUDA kernels, static), `b60_freq.txt` (GPU clock during the runs), `b60_contended.txt` (segments that overlapped another job and were repeated) |

Note: the PTX / SASS listings (`results/cuda_codegen/`, about 9 MB) are not kept in the repository; `scripts/cuda_codegen.sh` regenerates them (no GPU needed). `results/cuda_sass_counts.txt` has the counts taken from them.

## Build and run: B60

Needs the oneAPI DPC++ compiler (tested with icpx 2026.2) and the Level Zero driver.

    source <oneapi>/setvars.sh
    scripts/build_b60.sh                    # all seven programs; or: scripts/build_b60.sh mem arith
    scripts/run_b60.sh                      # everything; or: scripts/run_b60.sh gemm_stages xmx

The flags are the engine's: `-fsycl -O2 -std=c++20 -ffp-contract=off -fno-fast-math
-Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off"`, plus `-fsycl-device-code-split=per_kernel`.

Each program can also be run directly, e.g. `bin/gemm_stages 34816x5120`. Environment:

| variable | meaning |
|---|---|
| `ARCH_ONLY=a,b` | run only the named sections of a program (see the usage comment at the top of each source) |
| `ARCH_BITS=6` or `8` | `gemm_stages`: code width in bits per trellis step (6 = 3-bit weights, 8 = 4-bit weights) |
| `ARCH_COMPILE_ONLY=1` | compile all kernels of the program (fills the persistent kernel cache) and exit without running anything on the GPU |
| `SYCL_CACHE_PERSISTENT=1` | keep compiled kernels between runs (set by the run scripts) |
| `B60_LOCK` | lock file used by `scripts/b60run.sh` (default `/tmp/b60.lock`) |
| `B60_BUSY_RE` | regular expression of process command lines that mean "the B60 is in use"; `b60run.sh` waits while one matches |

The programs never use the large register file mode (`grf_size<256>`). `mem` allocates 1.5 GiB of device memory,
the others well under 1 GiB.

Generated code (nothing runs on the GPU):

    NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 ARCH_COMPILE_ONLY=1 IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=/tmp/igc_dump/ bin/gemm_stages
    scripts/igc_asm_count.py /tmp/igc_dump k_smallILi6ELi8ELi5E      # 3-bit codes, 8 rows, stage F
    scripts/igc_loop_body.py /tmp/igc_dump k_wideILi0E

Kernel names in the dumps are mangled: `k_smallILi<BITS>ELi<rows>ELi<stage 0..6>E[Li1E = planar codes, Li2E = planar + direct offsets, Li3E = + two tables]`,
`k_bigILi<BITS>ELi<TR>ELi<stage>E`.

## Build and run: RTX 5060 Ti

Needs the CUDA toolkit (tested: 13.1, target `sm_120`).

    NVCC=<cuda>/bin/nvcc scripts/build_cuda.sh
    scripts/run_cuda.sh                     # refuses to run while another compute process uses the GPU (FORCE=1 overrides)

Generated code of the upstream engine's GEMM kernels (no GPU needed):

    NVCC=<cuda>/bin/nvcc CUOBJDUMP=<path>/cuobjdump scripts/cuda_codegen.sh <upstream source tree> results/cuda_codegen
    scripts/sass_count.py results/cuda_codegen/<file>.sass <function name pattern>

`results/b60_mem_data.csv` is the `data` section of `mem` run on its own (`ARCH_ONLY=data bin/mem`);
`results/b60_gemm_stages_repeat.csv` holds repeated segments. Binaries go to `bin/` (ignored by git).

## Reading the results

* Times are the best of 5 repetitions. `ms_queued` = many launches, one wait (kernel time when the GPU never idles);
  `ms_sync` = launch + wait every time (adds the launch / completion overhead a pass of the engine pays when it waits).
* `Gweights_per_s` = matrix elements (N x Kc) per second for ONE launch regardless of the number of input rows M.
* One ESIMD thread is one hardware thread with explicit 16-lane vectors. One CUDA thread is one lane of a 32-lane warp.
  The CUDA result files use the same column names where the quantity is the same; the notes in REPORT.md say how the
  thread counts map.
* A result that could not be measured is marked as such in REPORT.md; no estimate is reported as a measurement.
