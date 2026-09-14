# CPU profiling: `rsp_multi` on file input (valgrind cachegrind + callgrind)

Measured 2026-09-14 with the fixed 2-channel harness below. Replaces
`rsp-potential-improvements.md` (roadmap preserved in git history); the
performance state reported here is post-LUT-shift, post-hand-rolled
decimator, post-pre-created-filters.

## What was profiled

- Binary `bin/rsp_multi.prof` (`make profile-rsp`: production codegen
  `-O3 -march=native` + `-g`, separate `obj/prof` tree — never profile
  the plain binary, annotation degrades to `???:` addresses).
- Input: 5 s slice of `run/429MHz_2Msps.cf32` (80 MB of 448 MB). A
  bounded slice keeps valgrind at tens of seconds while being
  statistically equivalent — the shares below reproduce the previous
  full-file baseline (10.40G Ir: kernel 63%, shift 19%) within a few
  points.
- Config: `-f 428.775M,429.525M -M fm -s 16k -r 48k` → capture 2 MS/s,
  per-channel ÷125 decimation to 16 kS/s, 16k→48k audio resample,
  outputs to `/dev/null,/dev/null`. File input is unpaced; chunk
  coalescing at the handoff under valgrind does not distort
  attribution — Ir per processed sample is constant.
- Two tools, same input, run in parallel: cachegrind
  (`--cache-sim=yes`) for flat Ir + cache events, callgrind for call
  counts and caller attribution.

## Results

Program total: **2,017,224,027 Ir** for 5 s of RF (~403 M Ir/s of
audio, both channels; ballpark 5–10% of one core natively). Cache
health is good (D1 miss 3.8%, LL 0.3%): compute-bound, nothing to gain
on the memory side.

Top consumers (whole program; *runtime = steady-state share, excluding
one-time startup, which is ~9.5% here and amortizes to zero in live
use):

| # | Function | Ir | % total | % runtime* | What it is |
|---|----------|-----------:|--------:|--------:|------------|
| 1 | `dsp_decimate_channel` (src/core/dsp.c:119) | 1,167.4 M | **57.9%** | 64.0% | Per-channel complex FIR decimation 2 M→16 k (÷125, **1001 taps**); scalar MAC loop `decim_dot` (dsp.c:69–76) = 47.6% + 10% loop overhead |
| 2 | `dsp_shift_frequency` (src/core/dsp.c:30) | 360.1 M | **17.9%** | 19.7% | Per-sample LUT NCO complex mul; **76% of all D1 read misses** — the 512 KB (64K-entry) phasor LUT does not fit 32 KB L1 |
| 3 | `__memcpy` (libc) | 162.4 M | 8.0% | 8.9% | 160 M in `deliver_buf` fan-out (device chunk copied into each channel), 2.4 M decimator tail slide |
| 4 | Audio resampler chain (liquid) | 75.6 M | 3.7% | 4.1% | `resamp_rrrf` 16k→48k: dotprod_run4 43.2 + firpfb 15.8 + resamp 10.6 + block wrapper |
| 5 | Startup filter design (one-time) | ~165 M | 8.2% | — | `dsp_init_filters` → `liquid_firdes_kaiser`: logf 61.1 + log 38.6 + lngammaf 37.0 + lnbesselif 11.6 + windowf 6.1 |
| 6 | FM discriminator | 15.7 M | 0.8% | 0.9% | `dsp_polar_discriminant` + `atan2f` (160k calls = 16 kS/s × 5 s × 2 ch) |
| 7 | `file_input_thread_fn` self (rsp backend) | 15.1 M | 0.7% | 0.8% | fread + CF32 unpack (×128) |
| 8 | Startup misc (one-time) | ~28 M | 1.4% | — | `shift_lut_build` (cexpf) 9.8 M, large `memset`s from `main` 18.6 M |
| 9 | Thread/pipeline glue | ~10 M | 0.5% | 0.5% | `demod_thread_fn` + `pipeline_process` self, condvar handoff |

Inclusive view: demod threads 81.8%, device/file thread 8.7%,
`main` 9.2% (startup). The DSP hot path is ~94% of runtime cost.

## Hotspots

1. **Decimation is ~⅔ of everything.** Cost driver: every 16 k-rate
   output sample is a full 1001-tap complex dot product (h_len =
   2·m·M+1 with m=4, M=125; ~239 k Ir per 8192-sample chunk, ~65
   outputs). The inner loop is deliberately scalar (dsp.c:56 — liquid's
   dotprod is a no-FMA SSE kernel; our 4-accumulator scalar-FMA chain
   already beats it ~3×), and the strided `xf[2*k]` complex access
   blocks gcc auto-vectorization. Biggest levers, in order:
   **(a)** factorized multi-stage decimation — per-stage tap counts
   scale with the stage's own factor, e.g. 5·5·5 ≈ 8× fewer MACs than
   one ÷125 stage (see the former roadmap doc for the full design and
   validation gates) — **[correction 2026-09-14: this 8× estimate was
   wrong; simulation in `docs/improvement-options.md` Option 1 shows
   the cascade costs 1.27× MORE MACs at equal per-stage shape and
   collapses far-zone alias rejection by ~60 dB. Rejected.]**;
   **(b)** a full-width AVX kernel on deinterleaved scratch
   (~3–4× on the remainder) — now the primary lever
   **[validated 2026-09-14: 3.8× measured, `tools/simd_decim_bench.c`;
   see `docs/improvement-options.md` Option 2]**.
2. **NCO LUT thrashes L1.** `dsp_shift_frequency` causes 22.5 M D1
   read misses (76% of the program's total); a quarter of its Ir is
   plausibly L1-miss latency. A 4K-entry LUT (32 KB, fits L1) with
   interpolation would recover most of the 17.9%.
3. **Fan-out memcpy (8%)** is structural to the one-device-many-
   channels model; refcounted zero-copy buffers would trim it but
   touches the chunk handoff that AGENTS.md says not to casually fix.
4. **Not worth touching:** FM demod (`atan2f`, 0.8%), audio resampler
   (4.1%), file I/O (<1%), glue (<1%).

## Reproduce and verify in one go

Everything below runs in ~1 minute and self-checks. Ir counts are
timing-independent, so the two tool runs may execute concurrently and
on a loaded machine without changing results.

```bash
make profile-rsp

# 5 s slice is enough; full 28 s file triples the runtime, same shares
head -c 80000000 run/429MHz_2Msps.cf32 > /tmp/opencode/prof_5s.cf32

ARGS=(-f 428.775M,429.525M -M fm -s 16k -r 48k
      -I /tmp/opencode/prof_5s.cf32:2M:429M -O /dev/null,/dev/null)

valgrind --tool=cachegrind --cache-sim=yes \
    --cachegrind-out-file=/tmp/opencode/cg.out \
    bin/rsp_multi.prof "${ARGS[@]}" 2>&1 | tee /tmp/opencode/cg.log
valgrind --tool=callgrind \
    --callgrind-out-file=/tmp/opencode/cl.out \
    bin/rsp_multi.prof "${ARGS[@]}" 2>&1 | tee /tmp/opencode/cl.log

# verification gate: the two tools must agree on program-total Ir
a=$(grep -oP 'I refs:\s+\K[0-9,]+' /tmp/opencode/cg.log | head -1 | tr -d ,)
b=$(grep -oP 'Collected : \K[0-9]+' /tmp/opencode/cl.log | tr -d ,)
python3 -c "a,b=int('$a'),int('$b'); d=abs(a-b)/a*100
print(f'cachegrind={a:,} callgrind={b:,} delta={d:.4f}%'); assert d < 0.01"

# analysis views
cg_annotate --no-annotate /tmp/opencode/cg.out          # per-function table
callgrind_annotate --tree=caller --threshold=100 --auto=no /tmp/opencode/cl.out
cg_annotate /tmp/opencode/cg.out                        # + hot source lines
```

Pass criteria for "ran and verified":

- Both runs print `IQ file input ... exhausted, exiting...` followed by
  `User cancel, exiting...` — the latter is the normal file-mode
  shutdown path, not an error.
- Totals agree within 0.01% (measured 0.0006%: 2,017,224,027 vs
  2,017,211,220). A large delta means the runs saw different data —
  usually a stale slice file or different args.
- Per-function sanity anchors: `dsp_decimate_channel` called 4,884×
  (= 2 channels × 2,442 deliveries), `atan2f` 160,000×
  (= demod rate × seconds × channels), `resamp_rrrf_execute` 480,000×
  (= 3× audio out). If these counts hold, the profile reflects the
  intended config.

### Pitfalls (each one cost a detour)

- **cachegrind 3.25 records Ir only by default.** Without
  `--cache-sim=yes`, `cg_annotate --show=D1mr` fails with
  ``--show event `D1mr` did not appear in `events:` line`` and the
  cache story is simply absent.
- **`cg_annotate --threshold` caps at 20** (percent) in 3.25; values
  like `99` abort with `invalid threshold value`. Just omit it.
  (`callgrind_annotate --threshold=100` is fine and useful with
  `--tree=caller`.)
- **Read the caller tree before chasing libm hits.** `logf`, `log`,
  `expf`, `lngammaf`, most `memcpy`/`memset` all belong to one-time
  startup (Kaiser window design, LUT build, static buffer init). The
  only per-sample libm call at runtime is `atan2f` in the FM
  discriminator.
- **Derive tap counts from Ir, not from `-s`.** 239k Ir per decimate
  call ÷ ~65 outputs = ~1001 taps → M=125, i.e. the decimator spans
  capture→channel rate in one stage. Easy to misread the pipeline as
  ÷11 (the 2M/181.8k snap) and mis-attribute.
- **libc/libm frames resolve only if debug symbols are installed**
  (here: Arch debug packages). `???:` paths for libc are harmless;
  `???:` for project code means you profiled a non-`-prof` binary.
- **`-O /dev/null,/dev/null` (one per channel) keeps TCP/UDP/file
  sinks out of the measurement**; multi-channel mode has no stdout.
- valgrind serializes threads and slows everything ~30–50×; do not
  read wall-clock from these runs (use `time bin/rsp_multi.prof ...`
  without valgrind for that), and don't E2E-listen under valgrind —
  scheduling distortions make audio judgment meaningless.
