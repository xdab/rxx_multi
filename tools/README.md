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
