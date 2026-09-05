#!/usr/bin/env python3
# Band-power profile + peak list of a raw IQ recording (CF32, interleaved
# float32 I/Q). Fast way to see what's inside a file before picking
# channel frequencies, e.g. after rsp_multi -R / rtl_multi -R capture.
#
# Usage:
#   iq_bands.py FILE RATE [CENTER] [SECONDS]
#
#   FILE     recording (CF32)
#   RATE     capture rate, e.g. 2M
#   CENTER   center frequency, e.g. 102.2M (default: offsets only)
#   SECONDS  how much of the file to scan (default 4, file start)
#
# Example:
#   iq_bands.py capture.cf32 2M 102.2M
import sys

import numpy as np


def atofs(s):
    s = s.lower()
    for suf, mul in (("g", 1e9), ("m", 1e6), ("k", 1e3)):
        if s.endswith(suf):
            return float(s[:-1]) * mul
    return float(s)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, rate = sys.argv[1], atofs(sys.argv[2])
    center = atofs(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    secs = atofs(sys.argv[4]) if len(sys.argv) > 4 else 4.0

    x = np.fromfile(path, dtype=np.complex64, count=int(rate * secs))
    if len(x) == 0:
        sys.exit("empty file")
    sp = np.abs(np.fft.fftshift(np.fft.fft(x * np.hanning(len(x))))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(len(x), 1 / rate))

    # Coarse band profile, 50 kHz bins, 0 dB = strongest bin
    nb = 50
    bins = sp[: len(sp) // nb * nb].reshape(-1, nb).mean(axis=1)
    bc = (np.arange(len(bins)) + 0.5) / len(bins) * rate / 1e3 - rate / 2e3
    db = 10 * np.log10(bins / bins.max())
    print(f"{path}: {len(x)} samples ({len(x) / rate:.1f} s @ {rate:.0f} Hz)")
    for i in np.argsort(db)[::-1][:12]:
        c = center + bc[i] * 1e3
        loc = f"{c / 1e6:9.3f} MHz" if center else f"{bc[i]:+8.1f} kHz"
        print(f"  {loc}  {db[i]:6.1f} dB  " + "#" * int(db[i]) + "+")

    # Fine peaks, >150 kHz apart
    print("peaks:")
    idx = np.argsort(sp)[::-1]
    seen = []
    for i in idx:
        if all(abs(f[i] - s) > 150e3 for s in seen):
            seen.append(f[i])
            c = center + f[i]
            loc = f"{c / 1e6:9.4f} MHz" if center else f"{f[i] / 1e3:+8.2f} kHz"
            print(f"  {loc}  {10 * np.log10(sp[i] / sp.max()):6.1f} dB")
        if len(seen) >= 8:
            break


if __name__ == "__main__":
    main()
