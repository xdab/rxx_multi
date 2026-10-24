# tools/

Helper scripts for hardware-free testing and CF32 recording analysis.
Both are standalone Python 3 (+numpy), no project build required.

## gen_test_iq.py

Synthesize a raw IQ test file (CF32, interleaved float32 I/Q, ±1.0 full
scale) for offline testing of `-I` file input mode — no SDR needed.

```
gen_test_iq.py OUT RATE CENTER DUR FREQ[:DEV[:MOD]] [FREQ[:DEV[:MOD]]...]
```

- `OUT` output file (CF32)
- `RATE` capture rate, e.g. `240k`
- `CENTER` center frequency, e.g. `100M`
- `DUR` duration in seconds
- `FREQ[:DEV[:MOD]]` FM carrier at absolute frequency `FREQ` with
  deviation `DEV` (default 5k) modulated by a tone at `MOD` Hz (default 1k)

Example — two test carriers, 2 s at 240 kHz:

```bash
tools/gen_test_iq.py /tmp/test.cf32 240k 100M 2 100M 100.05M:5k:2.1k
bin/rtl_multi -I /tmp/test.cf32:240k:100M -f 100M -M fm -s 24k -r 48k > out.raw
```

A channel tuned to `FREQ` must demodulate to a sine peak at exactly
`MOD` Hz — check with an FFT of the output.

## gen_scene_iq.py

Synthesize a deterministic multi-channel CF32 "saturated spectrum" scene:
a fixed channel plan with every modulation the receiver supports, a
unique audio tone per channel, a weak off-channel carrier, and a seeded
noise floor. Unlike `gen_test_iq.py` (ad-hoc, single-modulation), the
scene is generated once and reused across many test runs — each run
demodulates the channels matching its `-M` mode, and the audio must come
out at exactly the per-channel tone frequency. The channel plan lives in
the `SCENE` constant (importable for a future regression driver).

```
gen_scene_iq.py OUT [--rate 2M] [--center 100M] [--seconds 2]
                    [--seed N] [--noise SIGMA] [--base-amp A]
```

Deterministic: fixed default seed, closed-form FM phase, seeded noise —
bit-reproducible run to run, so `compare_audio.py` bit-exactness is a
meaningful criterion. The default 2M rate is accepted by both backends.

Example — FM channels end to end (no hardware):

```bash
tools/gen_scene_iq.py /tmp/scene.cf32
bin/rtl_multi -I /tmp/scene.cf32:2M:100M -f 99.1M,99.4M,99.85M,100.6M \
    -M fm -s 24k -r 48k -O fm1.raw,fm2.raw,fm3.raw,fm4.raw
```

Each output must show an FFT peak at 440 / 880 / 1560 / 2640 Hz. Run the
scene through `iq_waterfall.py` for a visual check of the plan.

## regression.py

Commit-gate demodulation suite: one case per modulation runs the receiver
in `-I` file mode over the matching scene channels, then each output is
checked mechanically — tone frequency within 10 Hz, prominence above the
band, every other scene channel's tone rejected — and the FSK channel's
raw output must concentrate around its deviation. No golden files: the
scene is deterministic, so the checks are self-contained. For refactor
work, `--update-ref` / `--ref` add a bit-exact comparison against a saved
reference decode (per binary + scene, under `~/.cache`; file mode is
deterministic, so reruns must match — only raw mode jitters by a few dozen
EOF samples, which is reported, not failed).

```
regression.py [--bin PATH] [--only SUBSTR] [--list] [--regen]
              [--seconds S] [--keep] [--update-ref] [--ref]
```

- `--bin` receiver binary (default `bin/rtl_multi` — the core is
  backend-identical and `-I` bypasses the device)
- `--only SUBSTR` restrict to cases whose label contains SUBSTR
- `--list` print the case matrix
- `--regen` regenerate the cached scene (default: reuse
  `/tmp/rxx_multi_scene-<hash>.cf32`, generate on first use)
- `--seconds S` scene duration (default 2)
- `--keep` keep the workdir with the raw outputs for
  `compare_audio.py` / `audio_hiss_metric.py` follow-ups
- `--update-ref` save this run's outputs as the reference decode
- `--ref` require bit-identical match to the reference

Exit status: 0 all pass, 1 any fail, 2 setup error.

Example:

```bash
make rtl && python3 tools/regression.py
# 5/5 passed in ~2 s
```

## iq_bands.py

Band-power profile + peak list of a CF32 recording. Fast way to see what
a `-R` capture contains before choosing channel frequencies.

```
iq_bands.py FILE RATE [CENTER] [SECONDS]
```

- `FILE` recording (CF32)
- `RATE` capture rate, e.g. `2M`
- `CENTER` center frequency (default: report offsets only)
- `SECONDS` how much of the file to scan from the start (default 4)

Example:

```bash
tools/iq_bands.py capture.cf32 2M 102.2M
```

Prints an ASCII band profile (50 kHz bins, 0 dB = strongest bin) and the
strongest narrowband peaks (>150 kHz apart).

## iq_waterfall.py

2D waterfall (spectrogram) plot of a CF32 recording with matplotlib.

```
iq_waterfall.py FILE RATE [CENTER] [SECONDS] [OUT.png]
```

- `FILE` recording (CF32)
- `RATE` capture rate, e.g. `912k`
- `CENTER` center frequency (default: frequency axis shows offsets)
- `SECONDS` how much of the file to plot from the start (default: all)
- `OUT.png` save to file instead of opening an interactive window

Example:

```bash
tools/iq_waterfall.py baseband.cf32 912k 172.956M 20 waterfall.png
```

## audio_hiss_metric.py

Objective junk metric for demodulator A/B: ratio of audio energy in the
3.5–7 kHz band vs 0–3.5 kHz, over the first N seconds, per file. The
above-band shelf is discriminator noise; comb structure is periodic
seam clicks. Prints a table and saves a bar+PSD plot.

```
audio_hiss_metric.py OUT.png REF.wav CAND.wav [CAND.wav ...]
                    [--seconds S] [--split HZ] [--lp HZ]
```

- `REF.wav` reference recording (gray, first bar), candidates follow
- `--seconds S` analyze only the first S seconds (default 7 — point it
  at a section with a clean signal)
- `--split HZ` band split (default 3500)
- `--lp HZ` apply a 129-tap lowpass at HZ to candidates only
  (default 8000 — matches what the channel bandwidth should pass;
  skipped for signals whose Nyquist is below it; `--lp 0` disables)

Example:

```bash
python3 tools/audio_hiss_metric.py junk.png raw.wav mine.wav --seconds 7
```

## raw2wav.py

Convert raw PCM output (S16_LE int16 — demodulated audio of both
programs) to a WAV file playable with anything.

```
raw2wav.py FILE [RATE] [CHANNELS] [OUT.wav]
```

- `FILE` raw PCM input (S16_LE)
- `RATE` sample rate (default 48000, the rtl_multi audio default)
- `CHANNELS` 1 = mono (default), 2 = stereo (raw demod mode)
- `OUT.wav` output file (default: input name with `.wav` extension)

Example:

```bash
rtl_multi -I baseband.cf32:912k:172.956M -f 173.325M -M fm -s 16k -r 48k -O ch.raw
tools/raw2wav.py ch.raw
# -> ch.wav, playable with paplay/play/anything
```
