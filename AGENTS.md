# AGENTS.md

rxx_multi is a multi-channel narrowband SDR receiver/demodulator: one radio
device, many independent demod chains, built from a single shared codebase
that compiles into two binaries — `bin/rtl_multi` (RTL-SDR / librtlsdr) and
`bin/rsp_multi` (SDRplay RSP / API v3). It is the 1:1 merge of two former
sibling projects; the core DSP is byte-identical to what both shipped. See
README.md for features, installation, and usage.

## Project layout

- `Makefile` — `make` builds both binaries; `make rtl` / `make rsp` build one;
  `make clean` removes `obj/` + `bin/`; `make install` → `/usr/local/bin`.
  Core sources are compiled once per backend (only difference: `-DAPP_NAME`).
- `include/` — shared headers: `types.h` (structs, constants, externs),
  `options.h` (`channel_t`, `options_t`, backend option hooks),
  `device.h` (backend interface + `capture_plan`), `pipeline.h`, `demod.h`,
  `dsp.h`, `output.h`, `thread.h` (thread fns, `safe_cond_signal` macro).
- `src/core/` — backend-independent code: `main.c`, `options.c`,
  `pipeline.c`, `demod.c` (FM/AM/USB/LSB/raw), `dsp.c`
  (shift/decimate/resample/filters), `thread_demod.c`, `thread_output.c`,
  `output_tcp.c` (multi-client broadcast), `output_udp.c` (fire-and-forget).
- `src/backend/rtl/` — `device.c` (librtlsdr lifecycle, gain, bias-T, ppm,
  direct sampling), `thread_device.c` (USB callback, `-I` file playback).
- `src/backend/rsp/` — `device.c` (SDRplay API v3 lifecycle, gRdB/LNA/AGC,
  rate snap), `thread_device.c` (stream/event callbacks, rechunking,
  `-I` file playback), private prototypes declared locally, not in `include/`.
- `tools/e2e_capture.py` — manual E2E test (RSP backend only, see below).

## Build & test

```
make            # both binaries: bin/rtl_multi, bin/rsp_multi
make rtl        # only RTL-SDR binary
make rsp        # only RSP binary
make clean
```

No test suite exists. Verification options:

- **File input mode (`-I`)** works with no hardware: feed any CF32 recording,
  e.g. `bin/rsp_multi -f 99.85M -M wbfm -I file.cf32:2M:100M -O out.raw`.
  Useful for smoke tests and DSP regressions.
- **E2E capture (manual, RSP):** `python3 tools/e2e_capture.py` — needs
  `sdrplay_apiService` running (`systemctl is-active sdrplay.service`), `nc`,
  `ffmpeg`. Launches three WBFM channels → TCP 8101-8103, records ~15 s per
  channel, converts to WAV in `/tmp/opencode`. Judge by ear against SDR++
  with the antenna in the same physical position.
- **RTL backend:** run against a dongle and compare by ear, same method.

## Architecture

Three-stage pipeline, one pthread per stage per channel:

1. **Device** — backend `thread_device.c` converts hardware samples to
   `float complex` (~±128 convention) and calls `deliver_buf`, which fans
   out to every `demods[]` (single-channel mode is the N=1 case).
   RTL delivers ~8192-pair USB chunks; RSP callbacks arrive in small dribbles
   (~1344 samples) and are accumulated into 8192-sample chunks first to
   preserve demod pacing. First ~300 ms after RSP `Init` are dropped (DC-cal
   transient). `-I` replaces the hardware source with a CF32 file read in the
   same chunk sizes, as fast as the pipeline consumes it (unpaced).
2. **Demod** — waits on condvar, runs `pipeline_process`: NCO shift → FIR
   decimation → demodulate → de-emphasis → DC block → audio
   resample.
3. **Output** — waits on condvar, writes PCM16 to TCP clients, UDP socket,
   file, or stdout.

Data flows through shared state structs (`device`, `demods[]`, `outputs[]`)
with mutex/condvar/rwlock sync; signal via the `safe_cond_signal` macro
(`thread.h`).

### Backend interface

Core never touches hardware APIs. `include/device.h` declares the contract,
implemented once per backend:

| Function                                                                                               | Role                                                                                   |
| ------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------- |
| `device_parse_option`                                                                                  | consume backend flags (`-g`, `-L`, `-p`, `-T`, non-universal `-E`) into `device_state` |
| `device_print_options`                                                                                 | backend help block in `options_usage()`                                                |
| `device_apply_options`                                                                                 | late decisions needing the full option set (gain vs `-I` conflicts)                    |
| `device_plan_capture`                                                                                  | width → `capture_plan` (rate snapping + downsample)                                    |
| `verbose_device_search` / `verbose_set_frequency` / `verbose_set_sample_rate` / `verbose_reset_buffer` | selection and configuration                                                            |
| `device_open` / `device_apply_settings` / `device_signal_exit` / `device_stream_stop` / `device_close` | lifecycle                                                                              |
| `device_thread_fn` / `device_init_state` / `file_input_thread_fn`                                      | streaming (`thread.h`)                                                                 |

`struct device_state` (in `types.h`) carries the union of all backend fields
(`gain` tenths-dB for RTL; `gain_rdb`/`lna_state`/`agc` for RSP;
`ppm_error`/`direct_sampling`/`biastee`/`mute` for RTL). Each backend uses
the fields it knows; the `dev` handle is `void *`, cast internally.

### Rate model

Core computes a requested capture `width` (channel span + guards); the
backend snaps it:

- **RTL:** rate = ceil-downsample × channel rate; valid ranges 225001–300000
  and 900001–3200000 Hz — the 300k–900k gap is snapped up to ≥900001. Hard
  max 2.4 MHz. Channel rate then equals `opts.rate_in` exactly.
- **RSP:** ADC always 8 MHz + API `ctrlParams.decimation` (×4/×2/×1), so the
  capture rate snaps up to exactly {2, 4, 8} MHz; `bwType` analog IF filter
  scales with rate (1.536/5/6/7/8 MHz). True channel rate = capture /
  downsample (e.g. 181,818 Hz for wbfm at 2M/11×) and is propagated to the
  pipeline and de-emphasis constants.

Because both backends yield `rate_channel` consistently, core sets
`input_rate == demod_rate == rate_channel`; `opts.rate_out` exists only for
CLI parity (`-s` sets both `rate_in` and `rate_out`).

### Sample formats

- RTL unpack: `(float)(byte - 127)`, ±128 full scale.
- RSP unpack: 8 MHz ADC + decimation gives right-justified 16-bit samples →
  `(float)xi / 256.0f`, deliberately keeping the ±128 convention so
  `output_scale` and the polar discriminator's DC-offset ratio match the RTL
  path. Do NOT use the ZIF unpack `(signed char)(x >> 8)`: direct ZIF at
  low fs is only 8 bits, left-justified — an 18 dB worse quantization floor.
- `-I FILE:RATE:CENTER`: CF32 (interleaved float32, ±1.0), scaled ×128.
- `-R FILE`: CF32 recording of the post-unpack stream ×(1/128); replay via
  `-I` multiplies ×128 again → bit-exact round trip. RATE/CENTER are
  runtime-derived and printed at startup as the exact `-I` invocation.

## SDRplay gotchas

- **Daemon required:** without `sdrplay_apiService`, `sdrplay_api_Open()`
  fails (`shm_open: No such file or directory`). Arch:
  `sudo systemctl enable --now sdrplay.service`.
- **API v3, not v2:** `sdrplay_api.h` / `sdrplay_api_*`. Old `mir_sdr_*`
  examples are v2 — don't mix.
- **Startup ramp:** ~1.5–2 s before first samples flow; the DC-cal transient
  is why `STARTUP_DROP_SAMPLES` (~300 ms) exists.
- Event callbacks `sdrplay_api_DeviceRemoved` / `DeviceFailure` set
  `do_exit = 1` so parked threads shut down cleanly.
- Gain is two controls: `-g` gRdB (0–59 dB IF reduction) and `-L` LNA state
  (RSP1: 0–2). Either given → manual (unset one defaults to 59 / 2, the
  user-validated "lowest overall levels"); neither → API AGC.

## Tech stack

- **C** (POSIX, `_POSIX_C_SOURCE 200809L`), pthreads (glibc), plain
  Makefile, gcc `-O3 -march=native -mtune=native`, `.clang-format` (Allman,
  4-space, no tabs).
- **librtlsdr** (RTL backend), **SDRplay API v3** `-lsdrplay_api` (RSP
  backend,
- **Liquid-DSP**: FIR decimation, resampling, NCO, IIR filters.

## Conventions

- **Naming:** `snake_case`; structs follow `name_state`/`name_buffer`;
  header guards `#ifndef NAME_H`; one module = one `src/` + `include/` pair
  (backend-private declarations stay inside `src/backend/<name>/`).
- **Init/cleanup:** `*_init()`/`*_cleanup()` per module; `memset` zeroing in
  init, resources freed in cleanup. Liquid-DSP objects are created lazily on
  first use in `dsp.c`.
- **Errors:** return -1 on error / 0 on success, print to stderr, no errno
  propagation. Option-parse errors print `ERROR: ...` and return -1 from
  `options_parse`.
- **Buffers:** `iq_buffer` / `real_buffer` carry `.len` — always set it.
  DSP scratch buffers: `static _Thread_local`.
- **GPLv2+** — source files carry the GPL header.

## Constraints

- **Core stays backend-agnostic:** no `#ifdef`, no hardware headers, no
  backend constants in `src/core/` or `include/` (except the backend-hook
  declarations in `options.h`/`device.h`). Backend-private stuff belongs in
  `src/backend/<name>/`.
- **CLI parity between the binaries is the contract:** universal flags
  (`-d -f -M -O -s -r -E dc/deemp -I -R -x`) behave identically; gain
  flags are backend-specific by nature (`-g/-p/-T` RTL, `-g/-L` RSP) and a
  foreign flag must fail loudly (`invalid option`) rather than silently.
- **Max 128 channels** (`FREQUENCIES_LIMIT`).
- **Multi-channel mode:** no stdout; per-channel `-O` (TCP/UDP/file),
  comma-separated.
- **No dynamic allocation in the hot path** — all buffers statically sized
  via `MAXIMUM_*` constants.
- **Global mutable state:** `device`, `demods[]`, `outputs[]`, `do_exit`,
  `freq_len` are file-scope in `main.c`, `extern` in `types.h` — inherited
  pattern, keep it.
- **Do not "fix" the chunk handoff casually.** The demod/output condvar
  handoff can coalesce (drop) chunks when a producer outruns a consumer;
  this is inherited, inaudible in live use, and makes file-mode output
  non-bit-reproducible. Adding per-stage backpressure deadlocks unless the
  shutdown path (main's single broadcast signal, drain-at-exit) is redesigned
  too — an attempted quick fix was reverted. If you take this on, design the
  full producer-consumer handshake including EOF drain first.

## Guiding principles

- **Keep it simple** — small focused tool; no abstraction layers beyond the
  one backend interface.
- **No new dependencies** without strong reason — librtlsdr, SDRplay API v3,
  Liquid-DSP, pthreads.
- **Single binary per backend, no config files** — everything via CLI flags.
- **One device, many channels** — the fan-out model; don't invent a new one.
- **Measure before optimizing** — DSP is `-O3 -march=native`; profile first.

## Glossary

- **gRdB** — RSP IF gain reduction in dB (0–59); higher = less gain.
- **LNA state** — RSP front-end LNA attenuation index (RSP1: 0–2).
- **ZIF** — zero-IF (direct downconversion) sample format.
- **CF32** — complex float32, interleaved I/Q, ±1.0 full scale.
- **gqrx/SDR++** — reference GUI receivers used to validate audio quality.
