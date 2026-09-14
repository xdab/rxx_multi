# Improvement options from CPU profiling

Derived from `docs/profiling-results.md` (2026-09-14 profile of
`rsp_multi`, 2-channel file input) and a fresh read of the codebase.
Each option states the problem, the idea, and an implementation
sketch — pointers, not designs. Verify everything with the harness in
`profiling-results.md` (`make profile-rsp` + the valgrind one-liner;
Ir totals are the currency, wall-clock only for sanity).

Runtime shares below are steady-state (startup excluded).

| Opt | Lever | Est. gain (runtime) | Risk |
|-----|-------|--------------------:|------|
| 1 | Multi-stage decimation | up to ~8× on item 1 (64%) | medium |
| 2 | AVX decimator kernel | ~3–4× on item 1 remainder | low |
| 3 | Small interpolated NCO LUT | most of item 2 (19.7%) | low |
| 4 | Fan-out copy trimming | ≤ item 3 (8.9%) | **high** |
| — | FM demod, audio resampler, file I/O | <1% each | rejected |

## Option 1 — factorized multi-stage decimation

- **Problem.** `dsp_decimate_channel` (src/core/dsp.c:119) is 64% of
  runtime. One ÷M stage means every output sample costs a full
  `2·m·M+1`-tap dot product (1001 taps at M=125). Tap count grows
  linear in M but cost per input sample grows linear in M too — the
  single stage is the worst case.
- **Idea.** Factor M into stages (125 = 5·5·5 or 25·5); run the
  cascade on the shifted chunk. Per-stage tap count is
  `2·m·M_stage+1` (41 taps per ÷5 stage), so total MACs per input
  sample drop from ~1001/125 ≈ 8.0 to ~3·41/5 ≈ 24.6/5·… — in short
  ~8× fewer for 5·5·5. Early stages also shrink the data the later
  stages touch.
- **Sketch.**
  - Generalize `channel_pipeline` (include/types.h:71-88): replace
    the single `decim_taps`/`decimator_tail`/`decim_rem` with a small
    fixed array of stage states (`DECIM_STAGES_MAX` ~4; tail sizing
    per stage is tiny — biggest stage has the smallest M_stage… check
    which stage order needs the `decimator_tail[2048]` budget).
  - `decimator_create` (dsp.c:90) becomes a loop designing one kaiser
    prototype per stage (`fc = 1/M_stage`, same As=35, m=4; first
    stage may want a tighter As since later stages add their own
    stopbands — pick per-stage As so the cascade meets ~35 dB end to
    end).
  - `dsp_decimate_channel` becomes: for each stage, dot-product
    decimate in place through the existing `temp_out` scratch
    (alternate two buffers), staging through `buffer->samples` as
    today; `decim_rem` carry becomes per-stage.
  - Factorization choice: prefer factors of M found by trial
    division, largest-first or balanced — a ÷2 stage at the *end* is
    nearly free but at the *start* halves nothing; put small factors
    last (operate on already-reduced rate). M=125 → 5,5,5 is
    balanced; M=11 (wbfm at 2M snap) is prime → single stage
    fallback, which the current code already is.
  - Keep `dsp_init_filters` (dsp.c:337) pre-creating all stages.
- **Verification.** `-I` file mode, compare demodulated audio
  numerically against the single-stage build (max/running difference
  of PCM16 output; expect small but nonzero — different filter), and
  by ear via `tools/e2e_capture.py`. Confirm anchor counts in the
  profile change as predicted (Ir in decimate drops ~8×).
- **Watch out.** Group delay = sum of stage delays; only matters if
  anything ever assumes the current delay (nothing in core does —
  demod is memoryless FM).

## Option 2 — vectorized decimator kernel (SIMD on deinterleaved data)

- **Problem.** `decim_dot` (dsp.c:56-86) is scalar by choice but its
  complex-interleaved access `xf[2*k]` defeats gcc auto-vectorization
  even with the 4-accumulator pattern; 47.6% of total Ir sits in this
  loop plus ~10% loop overhead.
- **Idea.** (a) Deinterleave each chunk once (Re/Im float arrays in a
  `_Thread_local` scratch pair, one pass over the 8192 samples) so the
  dot products become two unit-stride real×real loops gcc can
  auto-vectorize with FMA; or (b) an explicit AVX2/FMA kernel. Either
  composes with Option 1 (applies to every stage's dot).
- **Sketch.**
  - Variant (a): in `dsp_decimate_channel`, after the (optional)
    stage loop entry, build `float re[N], im[N]` scratch; rewrite
    `decim_dot` as two plain loops `ar += h[k]*re[k+off]`,
    `ai += h[k]*im[k+off]` — check with `gcc -fopt-info-vec` that
    they actually vectorize; keep the scalar tail loop.
  - Variant (b): hand kernel `_mm256_fmadd_ps` over 8 taps/iter, h
    broadcast per lane group; guard with `__builtin_cpu_supports` or
    just `-march=native` (already the build flag — a compile-time
    target is acceptable per Makefile).
  - The deinterleave pass itself is ~2 stores/sample — amortized over
    `h_len` MACs per output it is noise; at M=125 only ~65 outputs
    reuse each deinterleave, at small M (post-Option-1 stages) it is
    reused by proportionally more outputs, still fine.
- **Verification.** numerical: compare `dsp_decimate_channel` outputs
  old vs new on the same `-I` input to within float rounding
  tolerance (not bit-exact: different summation order); then the
  profile delta.
- **Watch out.** ordering of FMA accumulations changes results
  slightly — same class of difference as liquid's run4 grouping,
  harmless, but don't chase bit-exactness.

## Option 3 — NCO LUT: 4K entries + interpolation

- **Problem.** `dsp_shift_frequency` (dsp.c:30-49) is 19.7% of
  runtime and 76% of all D1 read misses: the 64K-entry
  (`SHIFT_LUT_BITS 16`, 512 KB) phasor table in `shift_lut[]`
  (dsp.c:18-21) cannot live in 32 KB L1; each access risks eviction
  and a quarter of the function's Ir is plausibly miss latency.
- **Idea.** Shrink to `SHIFT_LUT_BITS 12` (4K entries, 32 KB — fits
  L1) and linearly interpolate between the two neighboring phasors
  using the next 12 phase bits as the fraction: `p = lut[hi] +
  frac·(lut[hi+1] − lut[hi])`. Interpolated magnitude dips slightly
  (≤ ~0.06% for 4K grid) — comparable to the tolerance already
  argued in the dsp.c:9-17 comment block; update that comment's
  spur math. Alternative without interpolation: 16K-entry table
  (128 KB) still misses L1; interpolation is the point.
- **Sketch.**
  - Keep the 32-bit accumulator and `shift_step` unchanged; per
    sample compute `idx = acc >> 20`, `frac = (acc >> 8) & 0xFFF`
    scaled to [0,1) — exact mask widths from the two bit-budgets
    (32 − BITS used for index, next 12 for fraction).
  - Wrap: `idx+1` masked with `(SIZE-1)` (phasor periodicity).
  - Cost per sample rises (~2 mul + adds) but all L1-resident; the
    win is the removed miss latency. If profiling shows the
    interpolated version is not clearly ahead, a second step is
    computing `cos/sin` pairs in SoA layout (separate cos[]/sin[]
    float arrays) to help the compiler keep both in registers.
- **Verification.** spectrum check: feed a strong single tone via
  `-I`, FFT the shifted output (e.g. dump after decimate with `-M raw`
  to a file), confirm no visible spur ridges above ~-80 dBc; plus the
  standard profile delta (expect D1mr to collapse, Ir in the function
  to drop by up to ~¾ of its miss share).
- **Watch out.** all channels share the one table; per-channel phase
  sequences stride it at unrelated offsets — interpolation must use
  each channel's own accumulator, no shared index tricks.

## Option 4 — trim the fan-out memcpy (flagged, high-risk)

- **Problem.** 8.9% of runtime is `__memcpy` in `deliver_buf`
  (src/backend/rsp/thread_device.c:62, mirrored in
  src/backend/rtl/thread_device.c:48): the device chunk is copied
  into every `demods[i].input` under that demod's rwlock.
- **Idea.** Let demods consume the device buffer in place (refcounted
  chunk or N-buffered ring), dropping the per-channel copy.
- **Sketch.** This is exactly the territory AGENTS.md fences off: the
  condvar coalescing handoff plus `wait_demods_drained` /
  `seq_processed` polling is load-bearing for shutdown; the previous
  backpressure attempt deadlocked. If taken on: producer-owned
  buffer pool (≥2 slots so the device may fill slot n+1 while demods
  read slot n), `seq_*` counters become per-slot, and the shutdown
  drain path in `main.c` must free slots, not just broadcast. Budget
  it as a redesign task with its own plan, not a patch.
- **Why it's last.** 8.9% for the largest blast radius of all four
  options, and the profile is compute-bound overall — Options 1+3
  alone target ~84% of runtime.

## Considered and rejected (matching the profile's own verdict)

- **FM discriminator / `atan2f`** (0.9%), **audio resampler chain**
  (4.1%), **file input** (0.8%), **thread glue** (0.5%): below the
  noise floor of any change above; leave alone.
- **Startup filter design** (9.5% here): one-time, amortizes to zero
  in live use; a tabulated Kaiser window would only complicate
  `dsp_init_filters` for a one-off win. Skip.
- **Cache-side work generally:** D1 miss 3.8%, LL 0.3% — except for
  the NCO LUT (Option 3), the memory system is healthy; all other
  levers must remove instructions, not misses.
