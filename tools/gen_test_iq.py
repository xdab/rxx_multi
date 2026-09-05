#!/usr/bin/env python3
# Synthesize a raw IQ test file (CF32, interleaved float32 I/Q, +-1.0 full
# scale) for hardware-free testing of -I file input mode.
#
# Usage:
#   gen_test_iq.py OUT RATE CENTER DUR FREQ[:DEV[:MOD]] [FREQ[:DEV[:MOD]]...]
#
#   OUT     output file (CF32)
#   RATE    capture rate, e.g. 240k
#   CENTER  center frequency, e.g. 100M
#   DUR     duration in seconds, e.g. 2
#   FREQ    channel frequency (absolute, like -f); generates an FM carrier
#   DEV     FM deviation in Hz (default 5k)
#   MOD     modulation tone frequency in Hz (default 1k)
#
# Example - two tones, replay at 240k centered on 100M:
#   gen_test_iq.py /tmp/test.cf32 240k 100M 2 100M 100.05M:5k:2.1k
#   rtl_multi -I /tmp/test.cf32:240k:100M -f 100M -M fm
#
# Check the demodulated output with an FFT: a channel tuned to FREQ must
# show a peak exactly at MOD Hz.
import sys

import numpy as np


def atofs(s):
    s = s.lower()
    for suf, mul in (("g", 1e9), ("m", 1e6), ("k", 1e3)):
        if s.endswith(suf):
            return float(s[:-1]) * mul
    return float(s)


def main():
    if len(sys.argv) < 7:
        sys.exit(__doc__)
    out, rate, center, dur = (sys.argv[1], atofs(sys.argv[2]),
                              atofs(sys.argv[3]), float(sys.argv[4]))
    chans = []
    for spec in sys.argv[5:]:
        parts = spec.split(":")
        freq = atofs(parts[0])
        dev = atofs(parts[1]) if len(parts) > 1 else 5e3
        mod = atofs(parts[2]) if len(parts) > 2 else 1e3
        chans.append((freq, dev, mod))

    n = int(rate * dur)
    t = np.arange(n) / rate
    sig = np.zeros(n, dtype=np.complex64)
    amp = 0.7 / len(chans)
    for freq, dev, mod in chans:
        phi = 2 * np.pi * dev * np.cumsum(np.sin(2 * np.pi * mod * t)) / rate
        sig += (amp * np.exp(1j * phi)).astype(np.complex64) \
            * np.exp(1j * 2 * np.pi * (freq - center) * t).astype(np.complex64)

    sig.tofile(out)
    print(f"wrote {out}: {n} samples @ {rate:.0f} Hz, center {center/1e6:.3f} MHz"
          f" ({n * 8} bytes, {dur:.1f} s)")


if __name__ == "__main__":
    main()
