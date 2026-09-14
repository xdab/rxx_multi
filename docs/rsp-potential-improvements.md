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

   **Update 2026-09-14: b, c, d are landed** (m=4/As=35 prototype +
   hand-rolled linear decimator in `dsp.c`). Fresh profile, same
   harness at capture scale (2 FM channels, 28 s 2 MS/s file input):
   program 18.76G -> 10.40G Ir (-45%), native user CPU 3.07 -> 0.92 s
   (-70%). New ranking: kernel 6.55G (63%, scalar FMA chains),
   shift LUT 2.02G (19%), memcpy 0.91G (9%); liquid's window
   machinery is deleted. Two-stage (a) is now the biggest lever and
   shrinks kernel and shift together.

   a. **Two-stage decimation** (this item): cheap wide-transition
      first stage (x2/x4 half-band - every other tap zero - or a
      multiplier-free CIC comb) at full rate, then the existing
      narrow kaiser at the reduced rate. The first stage's transition
      band is huge relative to the ~1.3% final channel, so it needs
      very few effective taps and the long filter runs on 2-4x fewer
      samples. Realistic 2-4x cheaper decimation. The tail/remainder
      state machine from the hand-rolled decimator generalizes to a
      per-stage version.
   b. **Cheaper prototype** — DONE 2026-09-14: `m=4, As=35`, taps
      1501 -> 1001 at M=125 (2 MS/s / 16 kHz plan). Callgrind: program
      18.76G -> 14.55G Ir (-22%), kernel -33%. A/B on the run/ capture:
      outputs match after the expected m*M group-delay shift (6 audio
      samples), SNR 37-39 dB with outliers only in startup warm-up;
      hiss shelf unchanged. Note the group-delay shift: any prototype
      change moves the output by (m_old - m_new) * M input samples.
   c. **Kill `windowcf_push`** — DONE 2026-09-14: hand-rolled
      linear-buffer decimator (`dsp_decimate_channel`); tail always
      holds h_len-1 history, `decim_rem` carries the stream position
      mod M so outputs stay at global multiples of M. Taps and window
      anchoring verified bit-exact against `firdecim_crcf_create_kaiser`
      by impulse test; output length exact. Watch the output-grid
      arithmetic: a per-chunk `floor(in/M)` without the remainder
      carry silently drops `M - (in mod M)` samples per chunk
      (0.8% audio compression at M=125, 8192-sample chunks).
   d. **SIMD dot product** — half done 2026-09-14: plain C with
      `restrict` + 4 interleaved accumulators (liquid run4 rounding
      depth) yields scalar `vfmadd231ss` chains under
      `-march=native` — already 3x wall-clock over liquid's no-FMA
      SSE. gcc declines to vectorize the interleaved-complex
      reduction; full 8-wide AVX needs manual deinterleave into
      re/im scratch. Not attempted (kernel is no longer the only
      hot spot; two-stage shrinks it more for less code).
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

4. **Pre-create pipeline filters.** — DONE 2026-09-14
   `dsp_init_filters` (dsp.c) builds kaiser decimator, resampler,
   de-emphasis and DC-blocker at channel setup (creator helpers shared
   with the lazy paths, which remain as safety net). Outputs bit-exact
   vs the lazy build. File-mode first-output latency unchanged
   (172 -> 183 ms: the init was hidden inside startup anyway and is now
   serialized in setup); the win is on the live path, where the spike
   no longer lands on the first chunks / client connect.

5. **Daemon health monitoring.**
   The failure mode above is invisible to our code: the API drops
   samples internally, and every stage of ours looks healthy. Comparing
   the configured capture rate against the actual chunk delivery rate
   (one counter, checked once per second) would turn a "mystery gap"
   into an actionable `WARNING: API delivering 1.15 of 2.00 MS/s`.
