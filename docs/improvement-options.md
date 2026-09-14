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
| 1 | Multi-stage decimation | **REJECTED by sim** — see below | — |
| 2 | AVX decimator kernel | **IMPLEMENTED: −28.5% total Ir** | low |
| 3 | Small interpolated NCO LUT | **REJECTED by sim** — see below | — |
| 4 | Fan-out copy trimming | ≤ item 3 (8.9%) | **high** |
| — | FM demod, audio resampler, file I/O | <1% each | rejected |

With 1 and 3 rejected and 2 implemented, nothing in this list should
be touched without a fresh profile: after Option 2 the decimator is no
longer the hotspot (`dsp_shift_frequency` 12.5% is, and it is measured
not worth optimizing; the fan-out memcpy is next at 5.6% but is the
fenced chunk-handoff territory).

## Option 1 — factorized multi-stage decimation (investigated, rejected)

**Status: killed by simulation on 2026-09-14** (standalone harness,
ported `decim_dot`/tail/rem semantics from dsp.c; M=125 = ÷5·5·5).
Measured against the production single stage (m=4, As=35):

| config (taps/stage) | cMAC/input | vs base | far-zone alias leak (base: −59.5 dB) |
|---------------------|-----------:|--------:|--------------------------------------|
| 125 (production) | 8.01 | 1.00 | −59.5 dB |
| 5·5·5, m=4,4,4 (41/41/41) | 10.17 | 1.27× | **+0.2 dB** (Δ +59.6) |
| 5·5·5, m=2,3,4 (21/31/41) | 5.77 | 0.72× | −0.2 dB (Δ +59.3) |
| 5·5·5, m=3,3,3 (31 each) | 7.69 | 0.96× | −0.1 dB (Δ +59.3) |
| 25·5, m=4,4 (201/41) | 8.37 | 1.05× | +0.1 dB (Δ +59.6) |
| 5·25, m=4,4 (41/201) | 9.81 | 1.23× | −6.0 dB (Δ +53.5) |

- **The "8× fewer MACs" claim was wrong.** It counted every stage's dot
  products at the final output rate (3·41 vs 1001 taps per output),
  forgetting that stage 1 must produce 1638 outputs per 8192-sample
  chunk, not 65. At equal per-stage shape the cascade costs
  2m·(1 + 1/5 + 1/25) ≈ 1.27× the single stage's 2m.
- **Every naive cascade destroys far-zone alias rejection** (+53 to
  +60 dB): a per-stage kaiser with fc = 1/M_stage only protects that
  stage's own near band, not the alias zones of downstream stages —
  some far-out tone rides a transition region straight into the
  passband. A "proper" multistage design (multiband early stages with
  stopbands at their alias frequencies) needs ≥ ~63-tap stage 1 →
  ≈ 15.6 cMAC/input ≈ 2× baseline — arithmetic cross-validated by the
  25·5 and 5·5·5 measured points. There is no win anywhere.
- **The single stage is already at the polyphase cost floor** for its
  selectivity class: h/M = 8.01 ≈ 2m + 1/M per input sample. No
  re-factorization can beat it at equal selectivity. If more
  selectivity is ever wanted, the lever is a proper cutoff (fc toward
  1/(2M), more taps), not staging.

Side-findings from the same harness:

- **liquid kaiser taps are not gain-normalized** (`liquid_firdes_kaiser`
  Σh ≈ M/2; liquid's own `firdecim_crcf_create_kaiser` ≈ M). The
  production decimator therefore has ~62× passband gain at M=125 —
  invisible to FM (`dsp_polar_discriminant` is phase-only) but fully
  visible to AM/USB/LSB (demod.c multiplies only `output_scale` = 1.0)
  and raw. Inherited behavior (the old liquid path had it too), not a
  regression — but worth a dedicated fix/normalization pass.
- The tail/rem/e-index bookkeeping ports cleanly to cascades
  (chunk-independence ~3e-7 rel err, brute-force semantics match ~1e-6)
  — proven mechanics if multi-stage is ever needed for other reasons.

Original sketch (superseded, kept for the record):

- **Problem.** `dsp_decimate_channel` (src/core/dsp.c:119) is 64% of
  runtime. One ÷M stage means every output sample costs a full
  `2·m·M+1`-tap dot product (1001 taps at M=125).
- **Idea (refuted).** Factor M into stages; per-stage tap count is
  `2·m·M_stage+1`.
- **Sketch.** Stage array in `channel_pipeline` (include/types.h:71-88),
  per-stage kaiser in `decimator_create` (dsp.c:90), chained runs of
  the existing in-place stage code, prime-M fallback to single stage.

## Option 2 — vectorized decimator kernel — VALIDATED (3.8×), ready to implement

**Status: IMPLEMENTED 2026-09-14** — variant D landed in
`src/core/dsp.c` (`decim_dot_avx2` / `decim_dot2_avx2`, pairs built in
`decimator_create`, freed in `pipeline_cleanup`, scalar fallback
preserved; guarded by `#if defined(__AVX2__) && defined(__FMA__)`).
Landed gates:

- A/B vs baseline binary, 5 s file slice, 2 ch FM: max PCM16 diff
  **1 LSB** on 0.03–0.06% of 240k samples/channel (summation order
  only); wbfm M=11 path bit-identical; `-R`+`-I` rejection unchanged
  (pre-existing option guard).
- Profile, 1 s slice (cachegrind): program total 403.4M → **288.4M
  Ir/s of RF (−28.5%)**; `dsp_decimate_channel` **57.9% → 8.3%**;
  `dsp_shift_frequency` now tops the table at 12.5% (measured
  ~1 ns/sample natively — leave it, see Option 3).
- Wall-clock: user CPU 0.170 → 0.134 s per 5 s slice (−21%).

Validated 2026-09-14 with `tools/simd_decim_bench.c` (Ryzen 3700X /
Zen 2, AVX2+FMA, exact production hot-path shape: 65 × 1001-tap
complex dots per 8192-sample chunk, chained inputs so nothing is
hoistable):

| variant | speedup | notes |
|---------|--------:|-------|
| S — production `decim_dot` (scalar, 4-acc) | 1.00× | 4.1 G cMAC/s, 15.8 µs/chunk |
| A — deinterleave + gcc auto-vec | **0.20×** | gcc *does* vectorize it (`-fopt-info-vec`: 32 B vectors) but the codegen is 5× slower than scalar — auto-vectorization is a dead end for this pattern |
| B — AVX2/FMA on interleaved data, pair-broadcast taps | 2.9× | no deinterleave pass; load-port bound (2 loads/FMA) |
| C — AVX2/FMA on deinterleaved data | 2.2× | deinterleave cost + plain vector FMAs |
| **D — B + tap reuse across two output windows** | **3.8–3.9×** | halves tap loads; 15.8→4.1 µs/chunk |

All variants numerically exact vs S within float reordering
(max abs 6.1e-5 on |y| ≈ 112 ≈ 5e-7 relative). Production impact
estimate: `decim_dot` is 47.6% of total Ir (+10% loop overhead) → 3.85×
on the kernel ≈ **35% total runtime reduction** (compute-bound, so
wall-clock follows Ir).

- **Problem.** `decim_dot` (dsp.c:56-86) is scalar; the strided
  `xf[2*k]` complex access blocks useful auto-vectorization (and the
  attempted auto-vec variant A actively regresses 5×).
- **Idea (variant D).** Explicit AVX2/FMA kernel over the interleaved
  complex window with taps pre-broadcast to lane pairs
  `(h0,h0,h1,h1,h2,h2,h3,h3)`; process output windows in PAIRS so each
  tap vector load feeds two FMAs (3 loads per 2 dots instead of 4) —
  that is what buys the extra 30% over B on a load-port-limited core.
- **Implementation pointers.**
  - `decimator_create` (dsp.c:90): build `__m256 h_pairs[h_len/4]`
    alongside `decim_taps` (8 KB at M=125; free in
    `pipeline_cleanup`).
  - `dsp_decimate_channel` (dsp.c:174): bulk loop steps outputs two
    at a time (window starts `e − tail_len + n*M` and `+(n+1)*M`);
    odd final output via the single-window kernel (variant B form);
    the ≤8 boundary outputs straddling `decimator_tail` keep the
    split-dot scalar form — 12% of dots, negligible either way.
  - Guard with `#if defined(__AVX2__) && defined(__FMA__)`, keep the
    scalar loop as the fallback path (Makefile's `-march=native`
    means the guard is on for all local builds; it keeps the source
    buildable elsewhere).
  - Everything stays float; results differ from the scalar build only
    by summation order — same class of change as the earlier switch
    to the 4-accumulator scalar form.
  - End-to-end gates: `-I`/`-R` round trip still sane, E2E listen
    (`tools/e2e_capture.py`), and the profile harness
    (`docs/profiling-results.md`) — expect the
    `dsp_decimate_channel` Ir share to drop from ~58% to ~30%.

## Option 3 — NCO LUT: 4K entries + interpolation (investigated, rejected)

**Status: killed by simulation on 2026-09-14** (`tools/nco_lut_bench.c`:
production shift loop vs 12-bit interpolated AoS/SoA, 12-bit direct,
and a table-free phasor recurrence; Ryzen 3700X / Zen 2). The premise
was that the 512 KB table's D1 misses cost time. They do not.

| variant | accuracy (dBc) | ns/sample | D1mr/sample | Ir vs V0 |
|---------|---------------:|----------:|------------:|---------:|
| V0 — 16-bit direct (production) | −85.1 | **1.03** | 1.13 | 1.00 |
| V1 — 12-bit interp, AoS | −130.3 | 2.37 | 0.32 | 2.00 |
| V2 — 12-bit interp, SoA | −130.3 | 1.33 | 0.43 | 0.78 |
| V3 — 12-bit direct | −61.1 | 0.92 | 0.33 | 1.00 |
| V4 — phasor recurrence, K=1024 | −101.8 | 2.32 | 0.13 | 1.11 |

(accuracy = total error vs double-precision ideal over 1M samples, so
it bounds every spur line; speed insensitive to a 160 KB inter-chunk
cache-thrash, i.e. the loop is not memory-latency-bound.)

- **Every accuracy-preserving variant is slower than production.**
  Interpolation works exactly as theorized (−130 dBc, theory −131) but
  costs 29% (SoA) to 130% (AoS) more wall-clock. The recurrence is
  serial-dependency-bound (~2 FMA latencies/sample). Only the direct
  12-bit lookup is faster — by 11% — and it drops spurs to −61 dBc,
  worse than liquid's shipped 1024-entry table (−66 dBc), for ~2% of
  total runtime. Bad trade.
- **Why the profile's attribution was wrong:** on Zen 2 the 512 KB
  table is L2-resident (private 512 KB L2); the ~14-cycle L2 latency
  behind the D1 misses is fully hidden by out-of-order execution
  across independent iterations, while the extra FP ops of the
  alternatives sit on the critical path. Cachegrind counts misses but
  does not price them — D1mr is a count, not a cost. The same caveat
  applies to the "76% of all D1 read misses" headline in
  `profiling-results.md`.
- **True native cost of `dsp_shift_frequency`: ~1.0 ns/sample**, i.e.
  2 channels × 2 MS/s ≈ 0.4% of one core — versus its 19.7% *Ir*
  share (valgrind's serialized instruction-count view). Leave it
  alone.
- The bench also validates the accumulator bookkeeping across chunk
  boundaries (nonzero start phase, ±225 kHz / 1 kHz offsets) — useful
  if the NCO is ever touched for other reasons.

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
  options, and the profile is compute-bound overall — Option 2 + 3
  target ~84% of runtime without touching it.

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
