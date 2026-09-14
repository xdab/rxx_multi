# RSP backend: potential improvements

Findings and roadmap from debugging periodic audio dropouts (0.9 s of
audio, then 0.7 s of silence, repeating) that affected only `rsp_multi`,
never `rtl_multi`, when streaming demodulated audio over TCP. This
document is self-contained: no knowledge of the original session
required.

## Background

- The RSP path never delivers samples itself: the SDRplay API daemon
  (`sdrplay_apiService`) decimates the 8 MHz ADC stream on the host CPU
  and hands small packets (~336 samples) to the client over IPC —
  roughly 6000 callbacks/s at a 2 MS/s capture rate.
- Instrumentation showed our pipeline fully able to keep up while the
  daemon was pinned at 100% of one core and delivered only ~1.15 of
  2.00 MS/s; the missing 43% was dropped silently inside the API ring —
  exactly the audible duty cycle. RTL-SDR is immune by architecture: no
  daemon, no host-side decimation, no IPC pump.
- Trigger: API decimation was set with `wideBandSignal = 1`, a mode
  documented for signals wider than 1.6 MHz. Fix shipped 2026-09:
  `wideBandSignal = 0` (rsp/device.c) — the correct narrowband path,
  also far cheaper in the daemon. Dropouts gone.

Lesson: on the RSP path the daemon is part of the real-time budget.
Treat daemon CPU like capture bandwidth — a resource that can run out
and fail *silently*.

Performance work is tracked with a fixed harness: callgrind
`--cache-sim=yes`, 2 FM channels (`-s 16k -r 48k`), 28 s of 2 MS/s
file input (`run/429MHz_2Msps.cf32`); wall-clock from the same command.
Harness baseline at the start of the 2026-09-14 session: 18.76G Ir,
3.07 s user CPU.

## DONE

- **Narrowband API decimation** (2026-09): `wideBandSignal = 0` — the
  root-cause fix above; daemon sustains the full callback rate.
- **LUT channel-shift oscillator** (2026-09-13): `dsp_shift_frequency`
  mixes via a 65536-entry phasor LUT + uint32 DDS accumulator in place
  of `nco_crcf_mix_block_down`. Shift stage 2225M -> 401M Ir (-82%).
- **Cheaper decimator prototype** (2026-09-14): kaiser m=6/As=40 ->
  m=4/As=35, taps 1501 -> 1001 at M=125. Program 18.76G -> 14.55G Ir
  (-22%); A/B shows the expected m*M group-delay shift, SNR 37-39 dB.
- **Hand-rolled linear-buffer decimator** (2026-09-14): liquid
  `firdecim`/`windowcf` replaced by a linear-buffer FIR (tail =
  h_len-1, `decim_rem` carries the output grid mod M; taps verified
  bit-exact by impulse test). Window machinery (~21% of program)
  deleted; kernel is scalar-FMA chains. Program 14.55G -> 10.40G Ir
  (-45% cumulative), native user CPU 3.07 -> 0.92 s (-70%).
- **Pre-created channel filters** (2026-09-14): `dsp_init_filters`
  builds decimator/resampler/de-emphasis/DC-blocker at setup; lazy
  paths remain as safety net. Outputs bit-exact; removes the
  first-chunk init stall (live path: right at client connect).

Current profile split (10.40G total): decimation kernel 6.55G (63%),
shift LUT 2.02G (19%), memcpy 0.91G (9%), rest < 5% each. LL miss rate
0.1% — compute-bound; memory-side work has nothing to give.

## PLANNED

### Factorized multi-stage decimator

Idea: a single kaiser doing 125x needs 1001 taps because its
transition band is designed against the final 16 kHz rate. In a
cascade, each stage only has to anti-alias for the band the *next*
stage keeps, and required tap count scales with the stage's own
factor, not the total M. Factorize M at create time (M is always an
integer by the rate model):

- peel factors of 2 into half-band-style stages (cheapest), then 3s
  and 5s; a prime remainder becomes the final kaiser stage (m=4).
  Cap the stage count.
- M=125 -> x5·x5·x5 with 11/21/41 taps: ~190M MAC vs 449M single-stage
  (2.4x fewer). M=76 -> x2·x2·x19 (final 153 taps). M=11 -> single
  stage, i.e. today's behavior: unfactorizable M degenerates to the
  status quo, so the fallback is free.
- Implementation reuses the hand-rolled decimator's per-stage
  machinery (taps + tail of h_len-1 + `rem` carry, causal anchor) as a
  small stage struct, looped through ping-pong scratch buffers;
  stages created at setup per the pre-create pattern.

Expected: kernel 6.55G -> ~2.7-3.2G Ir; program 10.40G -> ~6.6-7.2G
(-30-35%); native ~0.92 -> ~0.60-0.70 s. The shift LUT can NOT move
after stage 1 for this capture plan: stage-1 output Nyquist (200 kHz
at x5) is below the ±375 kHz channel offsets, and 125 is odd so an x2
first stage breaks the integer chain — that bonus exists only for
plans with generous guards.

Validation gates, in order:

1. numpy composite check: design the identical per-stage prototypes,
   cascade the responses; require composite passband ripple <= ~0.5 dB
   and final-rate stopband met for the real M values (11, 16, 76, 125,
   plus one low-rate plan). Freeze the per-stage m/As rules only after
   this passes.
2. C build; output length must be exactly 1346688 frames on the
   harness file — the remainder-carry gotcha now lives in N stage
   state machines.
3. callgrind Ir + wall-clock vs baseline; the MAC ratio should show.
4. Audio A/B: alignment scan (composite group delay differs from the
   single stage), expect ~35-45 dB in-band SNR and the hiss shelf at
   the current 0.0026 level.

### Full-width AVX decimation kernel

Today's kernel compiles to scalar `vfmadd231ss` chains (4 interleaved
accumulators): 2 flops/instr with good ILP — already 3x wall-clock
over liquid's no-FMA SSE — but gcc declines to auto-vectorize the
interleaved-complex reduction. Plan: deinterleave each chunk into
re/im scratch (one extra pass, ~0.15G Ir), then two unit-stride
reductions that gcc reliably vectorizes to 8-wide `vfmadd231ps`.

Expected: kernel Ir per MAC ~3-4x down; compounds multiplicatively
with the cascade above (kernel lands around 0.5-1G Ir after both).
Sequencing matters: land the cascade first, then this works on a much
smaller remainder.

## IDEAS

Unplanned and unimplemented; directions to think in if the need
materializes.

- **RSP API x8 decimation (1 MS/s snap point)**: the ADC always runs
  at 8 MHz; x8 would put 1 MS/s in the snap set, halving per-channel
  demod cost and doubling the 4.1 ms chunk budget — the strongest
  real-time argument on weak machines. Costs: rate-model surgery with
  RTL-parity care, the daemon's host-side decimation work doubles (the
  fragile part!), and the x8 path needs an output-quality A/B.
  Invisible in file-mode profiling.
- **Shared coarse decimation before the fan-out**: one early stage for
  all channels amortizes its cost across N. Geometry-limited: a shared
  stage's output Nyquist must exceed the largest channel offset (here
  ±375 kHz caps it at x2, and odd M breaks the integer chain), so this
  only pays on plans with generous guards and many channels.
- **Emergency low-rate mode** (fsHz = 2 MHz, API decimation off):
  removes all host-side decimation work if the daemon is ever the
  bottleneck again. Known and accepted cost: RSP1 low-rate ZIF is
  8-bit left-justified (~18 dB worse quantization floor). A fallback,
  never a default.
- **Daemon health monitoring**: compare configured capture rate vs
  actual chunk delivery rate (one counter, checked once per second)
  and WARN on deficit — the API drops samples silently while every
  stage of ours looks healthy. Pure observability, tiny effort.
- **Evaluated and rejected: liquid `msresamp_crcf`**. Measured 43-49
  Msamp/s per channel, flat across M=11/76/125 — the fractional
  `resamp` tail stage works at input-rate filter cost and the halfband
  chain never materializes for odd M — vs ~100+ Msamp/s for our
  kernel: a 2-4x regression, plus a fractional output grid where the
  rate model expects exact integer decimation. (`msresamp2` is dyadic
  only, M=2^k.) Liquid's `resamp_rrrf` stays in the audio path
  (16k -> 48k), where the ratio is genuinely fractional and the
  polyphase is the right tool.
