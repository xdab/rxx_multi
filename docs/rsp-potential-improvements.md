# RSP backend: potential improvements

Findings from debugging periodic audio dropouts (0.9 s of audio, then
0.7 s of silence, repeating) that affected only `rsp_multi`, never
`rtl_multi`, when streaming demodulated audio over TCP. This document
records the root cause, the fix that shipped, and the improvement ideas
that were left on the table, with the reasoning behind each. It is
self-contained: no knowledge of the original debugging session required.

## Root cause (as measured, 2026-09)

- The RSP path never delivers samples itself: the SDRplay API daemon
  (`sdrplay_apiService`) decimates the 8 MHz ADC stream on the host CPU
  and hands small packets (~336 samples, i.e. 168 us of signal) to the
  client over IPC — roughly 6000 callbacks/s at a 2 MS/s capture rate.
- Instrumentation (temporary, since removed) showed our pipeline fully
  able to keep up (demod + output + TCP well inside the 4.1 ms/chunk
  budget), while the daemon was pinned at 100% of one core and delivered
  only ~1.15 MS/s instead of 2.0. The missing 43% was dropped inside the
  API's ring — silent, unrecoverable, and exactly the audible duty cycle.
- RTL-SDR is immune by architecture: librtlsdr moves USB data straight
  into our process. No daemon, no host-side decimation, no IPC pump.
- The trigger was our own config: API decimation was set with
  `wideBandSignal = 1`, a mode documented for signals wider than 1.6 MHz.
  **Fix shipped:** `wideBandSignal = 0` (rsp/device.c) — the correct
  narrowband decimation path, which is also far cheaper in the daemon.
  With it, the daemon sustains the full 5952 callbacks/s and the
  dropouts disappear.

Lesson: on the RSP path the daemon is part of the real-time budget.
Treat daemon CPU like capture bandwidth — a resource that can run out
and fail *silently*.

## Potential follow-ups

0. **DONE — LUT channel-shift oscillator (landed 2026-09-13).**
    `dsp_shift_frequency` now mixes with a 65536-entry phasor LUT +
    uint32 DDS accumulator instead of `nco_crcf_mix_block_down`, in
    place (temp-buffer memcpy dropped). Callgrind, 2 FM channels, 10 s
    912 kHz file input: shift stage 2225M -> 401M Ir (-82%), whole
    program 5.26G -> 3.30G Ir (-37%); cachegrind: D refs -55%, LL
    misses unchanged. Next hot spot is the decimator (item 2).

   ### Decimator options (post-LUT profile, 2026-09-13)

   The kaiser `firdecim` is now ~70% of CPU (`dotprod_crcf_run4` 54%
   + `windowcf_push` 11.6% + `firdecim_crcf_execute` 4.7%). Options,
   best value first:

   a. **Two-stage decimation** (this item): cheap wide-transition
      first stage (x2/x4 half-band - every other tap zero - or a
      multiplier-free CIC comb) at full rate, then the existing
      narrow kaiser at the reduced rate. The first stage's transition
      band is huge relative to the ~1.3% final channel, so it needs
      very few effective taps and the long filter runs on 2-4x fewer
      samples. Realistic 2-4x cheaper decimation.
   b. **Cheaper prototype**: `As=40, m=6` is generous for 12 kHz NBFM
      voice; `m=4, As=35` cuts taps by a third (913 -> 609 at M=76).
      Verify with the A/B harness on clean-signal windows.
   c. **Kill `windowcf_push`**: liquid's firdecim copies every input
      block into an internal circular window; we already handle
      partial blocks via `decimator_tail`. A hand-rolled linear-buffer
      decimator (or restructured block feeding) removes that copy.
   d. **SIMD dot product**: `dotprod_crcf_run4` is a 4-wide SSE-era
      kernel; a plain-C loop with `restrict` + separate accumulators
      auto-vectorizes to AVX2/AVX-512 FMA under `-march=native`
      (typically 2-4x on the inner kernel). Combines with (c).
   e. **Shared coarse decimation before the fan-out**: in multi-channel
      mode, if the channel plan fits in a reduced band, one coarse
      decimation for all channels amortizes stage-1 cost across N
      channels instead of per channel.
   f. **RSP only - item 1**: API-side x8 decimation (1 MS/s snap
      point) halves the input rate before it reaches us; rate-model
      surgery, not DSP-internal.

1. **Decimation x8 for narrow captures (1 MS/s snap point).**
   The RSP1 ADC always runs at 8 MHz; with x1/x2/x4 decimation the
   capture rate snaps up to {8, 4, 2} MHz, so even a ~0.5 MHz channel
   plan pays a 2 MS/s capture and the tight 4.1 ms demod budget that
   comes with it. The API accepts larger factors; x8 would put 1 MS/s in
   the snap set, halving per-channel demod CPU. The margin after the fix
   is thin (demod wall cycle ~4.2 ms vs the 4.096 ms budget at 2 MS/s),
   so this is the cheapest way to widen it. Caveats: touches the rate
   model (`device_plan_capture`, RTL parity contract), and the x8 path's
   output quality needs an A/B against x4. Note it lightens *our* CPU;
   the daemon's IPC callback rate is set by its packet granularity and
   may not improve.

2. **Restore demod headroom (two-stage decimation).**
   The channel decimator is a single kaiser FIR run at the full capture
   rate. Splitting it (e.g. cheap x8 half-band first, then the existing
   narrow x16 stage) cuts per-sample input cost several-fold on every
   backend. Pure DSP-internal change, no behavior or interface impact;
   protects weak machines (Raspberry Pi class) where the lossless
   producer handoff couples all channels to the slowest consumer.

3. **Emergency low-rate mode (fsHz = 2 MHz, API decimation off).**
   If the daemon is ever the bottleneck again, running the ADC at 2 MHz
   natively removes all host-side decimation work. Known and accepted
   cost: RSP1 low-rate ZIF is 8-bit left-justified (~18 dB worse
   quantization floor). Acceptable for FM voice in a pinch; not a
   default.

4. **Pre-create pipeline filters.**
   Demod filter objects (kaiser decimator, resampler) are created lazily
   on the first processed chunk — an ~80 ms one-time spike per channel,
   visible as a stall right when a TCP client connects. Creating them at
   channel setup removes the spike. Small, easy win.

5. **Daemon health monitoring.**
   The failure mode above is invisible to our code: the API drops
   samples internally, and every stage of ours looks healthy. Comparing
   the configured capture rate against the actual chunk delivery rate
   (one counter, checked once per second) would turn a "mystery gap"
   into an actionable `WARNING: API delivering 1.15 of 2.00 MS/s`.
