[![CI](https://github.com/xdab/rxx_multi/actions/workflows/ci.yml/badge.svg)](https://github.com/xdab/rxx_multi/actions/workflows/ci.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE.md)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20me-13C3FF?logo=kofi&logoColor=white)](https://ko-fi.com/xdabx)
[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-GitHub%20Sponsors-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/xdab)

# rxx_multi

Multi-channel narrowband SDR receiver and demodulator with two backends from
one codebase: `rtl_multi` for RTL-SDR dongles and `rsp_multi` for SDRplay RSP
devices. Monitors multiple frequencies simultaneously from a single receiver
and streams demodulated audio over the network.

## Why

SDR tools typically tune to one frequency at a time. rxx_multi lets you
monitor an entire band — multiple ham repeaters, weather stations, or
aircraft frequencies — from a single device, each channel with its own
modulation mode and network output. The two supported backends cover both a
cheap wide deployment (RTL2832U dongles) and a higher-performance option
(SDRplay RSP: up to 8 MHz capture, real tuner, hardware decimation, no PPM
drift to babysit).

## Features

- Simultaneous multi-channel reception from a single device
- Per-channel modulation: FM, wideband FM, AM, USB, LSB, raw I/Q
- Per-channel output: TCP server, UDP client, file, or stdout
- Frequency-shift tuning — channels can be anywhere within the capture bandwidth
- FM de-emphasis filter (75us) and DC blocking filter
- Raw IQ file input (`-I`) and recording (`-R`) for offline testing and 1:1 replay
- Backend-native gain control (see Options)
- Clean shutdown on device unplug or device failure
- Low CPU usage via Liquid-DSP optimized FIR decimation and NCO mixing

## Dependencies

Common:

- [Liquid-DSP](https://github.com/jgaeddert/liquid-dsp)
- pthreads (glibc)

RTL backend (`rtl_multi`):

- [librtlsdr](https://github.com/osmocom/rtl-sdr)

On Debian/Ubuntu:

```bash
sudo apt install librtlsdr-dev libliquid-dev
```

RSP backend (`rsp_multi`):

- [SDRplay API v3](https://www.sdrplay.com/) (`sdrplay_api.h`, link
  `-lsdrplay_api`) — on Arch, the `libsdrplay` package (AUR)
- The `sdrplay_apiService` daemon must be running; without it
  `sdrplay_api_Open()` fails with `shm_open: No such file or directory`:

```bash
sudo systemctl enable --now sdrplay.service
```

## Building

```bash
make          # both binaries: bin/rtl_multi and bin/rsp_multi
make rtl      # only the RTL-SDR binary
make rsp      # only the RSP binary
sudo make install
```

Each binary links only its own hardware library; building the RTL binary
needs no SDRplay installation and vice versa.

## Windows (MSYS2 UCRT64)

Build with the [MSYS2](https://www.msys2.org/) UCRT64 toolchain:

```bash
pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-rtl-sdr mingw-w64-ucrt-x86_64-fftw
```

Liquid-DSP is not packaged for MSYS2 — build it static once (the
`patches/` file carries the two UCRT fixes, `strsep` and `getrusage`):

```bash
curl -sL https://github.com/jgaeddert/liquid-dsp/archive/refs/tags/v1.8.3.tar.gz -o /tmp/liquid.tar.gz
tar xzf /tmp/liquid.tar.gz -C /tmp
patch -d /tmp/liquid-dsp-1.8.3 -p1 < patches/liquid-dsp-1.8.3-mingw.patch
cmake -S /tmp/liquid-dsp-1.8.3 -B /tmp/liquid-dsp-1.8.3/build -G "MSYS Makefiles" \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_EXAMPLES=OFF -DBUILD_AUTOTESTS=OFF -DBUILD_BENCHMARKS=OFF \
      -DCMAKE_INSTALL_PREFIX=/ucrt64
cmake --build /tmp/liquid-dsp-1.8.3/build -j4
cmake --install /tmp/liquid-dsp-1.8.3/build
```

Then `make` as on Linux. For the RSP backend, the SDRplay API is a system
install (not a pacman package); its location is anchored by
`SDRPLAY_HOME`, which defaults to `C:/Program Files/SDRplay/API`.

`make all` also stages the runtime DLLs beside the binaries (`dlls`
target: `librtlsdr`, `libusb-1.0`, `libwinpthread`; derived from each
image's import table, so the set self-adjusts). Static liquid means the
resulting `bin/` runs standalone — no toolchain on `PATH` needed.
`sdrplay_api.dll` is deliberately not staged: its EULA grants no
redistribution, so RSP users install the SDRplay API themselves. CI runs
the same shape as a `windows-build` job: UCRT64 build + the regression
suite from `bin/`.

## Usage

Single channel:

```bash
rtl_multi -f 145.5M -M fm -O tcp:8000
```

Multi-channel:

```bash
rsp_multi -f 145.5M,146.52M,433.0M -M fm,am,usb -O tcp:8000,tcp:8001,tcp:8002
```

Wideband FM (broadcast radio):

```bash
rtl_multi -f 101.1M -M wbfm -O tcp:8000
```

Receive audio with netcat:

```bash
nc localhost 8000 | aplay -r 24k -f S16_LE -t raw -c 1
```

Monitor two ham repeaters:

```bash
rtl_multi -f 145.5M,146.52M -M fm -g 40 -s 12k -r 24k \
  -O tcp:8000,tcp:8001
```

Mixed modulation modes with file output:

```bash
rsp_multi -f 145.5M,520k -M fm,am -O fm_ch1.raw,tcp:8000
```

### Options

Universal flags (both binaries):

| Flag | Description                                                                           | Default |
| ---- | ------------------------------------------------------------------------------------- | ------- |
| `-d` | Device index or serial (exact/prefix/suffix match)                                    | 0       |
| `-f` | Frequencies, comma-separated for multi-channel (k/M/G suffixes)                       | -       |
| `-M` | Modulation per channel: fm, wbfm, am, usb, lsb, raw                                   | fm      |
| `-O` | Output per channel: `tcp:port`, `udp:host:port`, or `FILENAME` (raw audio; no commas) | stdout  |
| `-s` | Channel sample rate (demod input)                                                     | 12k     |
| `-r` | Audio output rate                                                                     | 48k     |
| `-E` | Enable option: `dc`, `deemp` (use multiple `-E` flags)                                | off     |
| `-I` | Raw IQ file input (test mode, no device): `-I FILE:RATE:CENTER`                       | -       |
| `-R` | Record raw baseband IQ to FILE (CF32, replayable with `-I`)                           | -       |
| `-x` | Exit gracefully after SECONDS of running                                              | off     |

Backend-specific flags — using a foreign flag fails with `invalid option`:

| Flag                       | Binary    | Description                                      | Default    |
| -------------------------- | --------- | ------------------------------------------------ | ---------- |
| `-g`                       | rtl_multi | Tuner gain in dB                                 | auto       |
| `-p`                       | rtl_multi | PPM frequency correction                         | 0          |
| `-T`                       | rtl_multi | Bias-T power on GPIO 0 (rtl-sdr.com v3 dongles)  | off        |
| `-E direct` / `-E direct2` | rtl_multi | Direct sampling on I/Q (HF)                      | off        |
| `-g`                       | rsp_multi | RSP IF gain reduction in dB (gRdB, 0-59)         | auto (AGC) |
| `-L`                       | rsp_multi | RSP LNA state, 0-2 on RSP1 (2 = max attenuation) | auto (AGC) |

On the RSP, giving either `-g` or `-L` selects manual gain; the unset control
defaults to gRdB 59 / LNA 2. Giving neither enables the API AGC. The RSP has
no `-p` (the API handles frequency correction) and no bias-T support yet
(`-T` is rejected).

### IQ file input / recording (test mode)

`-I` plays a raw IQ recording through the normal demod pipeline with no
device attached — useful for testing channel configs offline. FILE is
interleaved float32 I/Q (CF32, ±1.0 full scale, e.g. an SDR++ baseband
recording); RATE and CENTER are the recording's capture rate and center
frequency (suffixes: `2M`, `102.5M`). Playback runs as fast as the pipeline
consumes it and exits cleanly at EOF. All channels must fit inside the
recorded span.

```bash
rsp_multi -I capture.cf32:2400k:145.3M -f 145.5M -M fm -O tcp:8000
```

`-R` records the live capture (all channels, whole span) to the same CF32
format and prints the exact `-I` invocation for 1:1 replay at startup:

```bash
rsp_multi -f 145.5M,146.52M -R capture.cf32 -O tcp:8000,tcp:8001
# ... later, replay offline:
rsp_multi -I capture.cf32:2000000:146010000 -f 145.5M,146.52M -O tcp:8000,tcp:8001
```

### Capture-rate snapping

Each backend snaps the required capture width to what its hardware supports:

- **RTL-SDR:** rates of 225001–300000 or 900001–3200000 Hz; a computed rate
  in the 300k–900k gap snaps up to ≥900001 Hz. Hard maximum ~2.4 MHz.
- **RSP:** the ADC always runs at 8 MHz with API decimation, so the capture
  rate is one of **{2, 4, 8} MS/s**; the request snaps up to the smallest
  fitting rate (a warning is printed).

The true channel rate is `capture / downsample` — on the RSP e.g. 181,818 Hz
for `-M wbfm` (2 MS/s, 11x) — and propagates to the DSP constants
(de-emphasis, resampler). Band-edge channels within ~500 kHz of a 2 MS/s RSP
capture show filter/decimator rolloff; prefer a 4 MS/s capture for wide spans.

## Architecture

Three-stage pipelined design, one pthread per stage per channel:

1. **Device** — reads I/Q from the hardware (librtlsdr async reads, or
   SDRplay API v3 stream callbacks), converts to float complex
2. **Demod** — frequency-shifts, decimates, demodulates, filters, resamples
   to audio
3. **Output** — streams PCM16 over TCP/UDP or writes to file/stdout

The core in `src/core/` is hardware-independent; each backend in
`src/backend/rtl|rsp/` implements the device interface from
`include/device.h` (lifecycle, gain model, option parsing, capture-rate
planning, sample conversion). Liquid-DSP provides FIR decimation,
arbitrary-rate resampling, NCO, and IIR filters. Device unplug / failure
events are reported and shut the process down cleanly.

See `AGENTS.md` for implementation notes, backend details, and constraints.

## License

GPLv3 — see [`LICENSE.md`](LICENSE.md).
