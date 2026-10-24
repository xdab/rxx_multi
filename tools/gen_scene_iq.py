#!/usr/bin/env python3
"""Synthesize a deterministic multi-channel CF32 IQ scene for hardware-free
receiver testing.

Unlike gen_test_iq.py (ad-hoc single-modulation FM tones), this builds one
canonical "saturated spectrum": a fixed channel plan with every modulation
the receiver supports, unique audio tones per channel, a weak off-channel
carrier, and a seeded noise floor.  Generate the scene once and reuse the
file across many test runs — a run demodulates the subset of channels that
match its -M mode, and the audio must come out at exactly the per-channel
tone frequency.

Usage:
  gen_scene_iq.py OUT [--rate 2M] [--center 100M] [--seconds 2]
                      [--seed N] [--noise SIGMA] [--base-amp A]

  OUT        output file (CF32, interleaved float32 I/Q, +-1.0 full scale)
  --rate     capture rate (default 2M — accepted by both backends; RTL
             hard max is 2.4M, RSP snaps to {2,4,8}M exactly)
  --center   center frequency (default 100M)
  --seconds  duration (default 2)
  --seed     RNG seed for noise floor + FSK bit pattern (default fixed;
             the scene is bit-reproducible run to run)
  --noise    complex Gaussian noise sigma per quadrature (default 0.01)
  --base-amp sum of all channel amplitudes at level 1.0 (default 0.7)

Channel plan (offsets from --center; amp = base-amp/len(channels) * level):

  offset     mod  tone   level  notes
  -900 kHz   fm    440    0.90  dev 2500
  -750 kHz   usb   620    0.60
  -680 kHz   cw      -    0.15  off-channel spur: nothing tunes here; a
                                 demodulated channel must reject it
  -600 kHz   fm    880    1.00  dev 5000
  -450 kHz   am   1050    0.80  depth 0.6
  -300 kHz   lsb  1230    0.55
  -150 kHz   fm   1560    0.85  dev 3500
  +150 kHz   fsk     -    0.70  2FSK 1200 baud, dev 2400 (raw-demod target)
  +300 kHz   am   1980    0.70  depth 0.5
  +450 kHz   usb  2300    0.60
  +600 kHz   fm   2640    0.95  dev 5000
  +750 kHz   lsb  2940    0.50
  +900 kHz   am   3300    0.60  depth 0.7

Example — verify the FM channels end to end (no hardware):

  tools/gen_scene_iq.py /tmp/scene.cf32
  bin/rtl_multi -I /tmp/scene.cf32:2M:100M -f 99.1M,99.4M,99.85M,100.6M \\
      -M fm -s 24k -r 48k -O fm1.raw,fm2.raw,fm3.raw,fm4.raw

Each output must show an FFT peak at 440 / 880 / 1560 / 2640 Hz.

Demod expectations per modulation (how the receiver sees these):
  fm   exp(j*phi), phi = dev/tone * (1 - cos(2*pi*tone*t)) — exact FM of a
       sine; polar discriminator yields a sine at the tone frequency
  am   (1 + depth*sin(2*pi*tone*t)) * carrier — envelope detection yields
       the tone; DC block removes the carrier
  usb  tone at +tone Hz relative to the channel (re+im projection)
  lsb  tone at -tone Hz relative to the channel (re-im projection)
  fsk  continuous-phase 2FSK, pattern from the seed (raw demod target)
"""

import argparse
import sys

import numpy as np

# (offset_hz, mod, tone_hz, level, param) — param: fm deviation Hz,
# am modulation depth, None otherwise.  Offsets keep >= 70 kHz to any
# neighbour so narrowband channels never overlap at a 2M capture.
SCENE = [
    (-900_000, "fm", 440, 0.90, 2500),
    (-750_000, "usb", 620, 0.60, None),
    (-680_000, "cw", 0, 0.15, None),
    (-600_000, "fm", 880, 1.00, 5000),
    (-450_000, "am", 1050, 0.80, 0.6),
    (-300_000, "lsb", 1230, 0.55, None),
    (-150_000, "fm", 1560, 0.85, 3500),
    (150_000, "fsk", 0, 0.70, 2400),
    (300_000, "am", 1980, 0.70, 0.5),
    (450_000, "usb", 2300, 0.60, None),
    (600_000, "fm", 2640, 0.95, 5000),
    (750_000, "lsb", 2940, 0.50, None),
    (900_000, "am", 3300, 0.60, 0.7),
]

FSK_BAUD = 1200

# Half-width a channel needs around its offset so the channel FIR never
# clips a neighbour at the span edges (largest tone/dev in the scene).
SPAN_GUARD_HZ = 15_000


def atofs(s):
    s = s.lower()
    for suf, mul in (("g", 1e9), ("m", 1e6), ("k", 1e3)):
        if s.endswith(suf):
            return float(s[:-1]) * mul
    return float(s)


def synth_channel(offset, mod, tone, param, amp, t, rate, rng):
    """Baseband for one channel, sampled on the shared time base t."""
    if mod == "cw":
        bb = np.ones_like(t)
    elif mod == "fm":
        # phi = 2*pi*dev * integral of sin(2*pi*tone*t) — closed form, so the
        # phase is bounded and bit-reproducible (no accumulator drift)
        phi = (param / tone) * (1.0 - np.cos(2 * np.pi * tone * t))
        bb = np.exp(1j * phi)
    elif mod == "am":
        bb = 1.0 + param * np.sin(2 * np.pi * tone * t)
    elif mod == "usb":
        bb = np.exp(1j * 2 * np.pi * tone * t)
    elif mod == "lsb":
        bb = np.exp(-1j * 2 * np.pi * tone * t)
    elif mod == "fsk":
        bits = rng.integers(0, 2, len(t) // FSK_BAUD + 2)
        nrz = np.repeat(bits.astype(np.float64) * 2.0 - 1.0, int(rate / FSK_BAUD))[: len(t)]
        bb = np.exp(1j * 2 * np.pi * param * np.cumsum(nrz) / rate)
    else:
        sys.exit(f"unknown modulation {mod!r} in scene")
    return amp * bb.astype(np.complex64) * np.exp(1j * 2 * np.pi * offset * t).astype(np.complex64)


def build_scene(rate, center, seconds, seed, noise, base_amp):
    """Synthesize the full scene: CF32 samples, no printing.

    Deterministic in (SCENE, rate, center, seconds, seed, noise, base_amp) —
    regression.py imports this to build/reuse the cached stimulus.
    """
    n = int(rate * seconds)
    t = np.arange(n, dtype=np.float64) / rate
    rng = np.random.default_rng(seed)

    sig = np.zeros(n, dtype=np.complex64)
    amp = base_amp / len(SCENE)
    for offset, mod, tone, level, param in SCENE:
        sig += synth_channel(offset, mod, tone, param, amp * level, t, rate, rng)

    sig += (noise * rng.standard_normal(n) + 1j * noise * rng.standard_normal(n)).astype(
        np.complex64
    )
    return sig


def main():
    ap = argparse.ArgumentParser(description="Deterministic multi-channel CF32 scene synthesizer.")
    ap.add_argument("out", help="output file (CF32)")
    ap.add_argument("--rate", default="2M", help="capture rate (default 2M)")
    ap.add_argument("--center", default="100M", help="center frequency (default 100M)")
    ap.add_argument("--seconds", type=float, default=2.0, help="duration in seconds (default 2)")
    ap.add_argument("--seed", type=int, default=20261024, help="RNG seed (default fixed)")
    ap.add_argument("--noise", type=float, default=0.01, help="noise sigma per quadrature")
    ap.add_argument("--base-amp", type=float, default=0.7, help="total channel amplitude sum")
    args = ap.parse_args()

    rate = atofs(args.rate)
    center = atofs(args.center)
    max_offset = max(abs(ch[0]) for ch in SCENE)
    if max_offset + SPAN_GUARD_HZ > rate / 2:
        sys.exit(
            f"ERROR: capture rate {rate:.0f} Hz cannot hold the scene "
            f"(needs {2 * (max_offset + SPAN_GUARD_HZ):.0f} Hz span)"
        )

    sig = build_scene(rate, center, args.seconds, args.seed, args.noise, args.base_amp)
    amp = args.base_amp / len(SCENE)
    print(
        f"{'offset':>10}  {'freq':>10}  {'mod':>4}  {'tone':>6}  {'level':>5}  {'param':>6}  {'amp':>6}"
    )
    for offset, mod, tone, level, param in SCENE:
        print(
            f"{offset / 1e3:>9.0f}k  {((center + offset) / 1e6):>9.3f}M  {mod:>4}  "
            f"{tone:>6.0f}  {level:>5.2f}  {param if param is not None else '-':>6}  "
            f"{amp * level:>6.3f}"
        )
    peak = float(np.max(np.abs(sig)))
    sig.tofile(args.out)
    print(
        f"wrote {args.out}: {len(sig)} samples @ {rate:.0f} Hz, center {center / 1e6:.3f} MHz "
        f"({len(sig) * 8} bytes, {args.seconds:g} s, peak {peak:.3f})"
    )


if __name__ == "__main__":
    main()
