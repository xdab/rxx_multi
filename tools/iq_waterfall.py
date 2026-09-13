#!/usr/bin/env python3
# 2D waterfall (spectrogram) plot of a raw IQ recording (CF32, interleaved
# float32 I/Q), e.g. from rtl_multi -R / rsp_multi -R.
#
# Usage:
#   iq_waterfall.py FILE RATE [CENTER] [SECONDS] [OUT.png]
#
#   FILE     recording (CF32)
#   RATE     capture rate, e.g. 912k
#   CENTER   center frequency, e.g. 172.956M (default: offsets only)
#   SECONDS  how much of the file to plot from the start (default: all)
#   OUT.png  save to this file instead of opening an interactive window
#
# Example:
#   iq_waterfall.py baseband.cf32 912k 172.956M 20 waterfall.png
import sys

import matplotlib.pyplot as plt
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
    secs = atofs(sys.argv[4]) if len(sys.argv) > 4 else 0.0
    out = sys.argv[5] if len(sys.argv) > 5 else None

    n = int(rate * secs) if secs > 0 else -1  # -1: whole file
    x = np.fromfile(path, dtype=np.complex64, count=n)
    if len(x) < 1024:
        sys.exit("not enough samples")

    # STFT: 1024-pt FFT, rows capped at ~1400 for a manageable image
    nfft = 1024
    step = max(1, (len(x) - nfft) // (1400 - 1))
    rows = (len(x) - nfft) // step + 1
    idx = np.arange(nfft)[None, :] + step * np.arange(rows)[:, None]
    seg = x[idx] * np.hanning(nfft)
    spec = 10 * np.log10(np.abs(np.fft.fftshift(np.fft.fft(seg, axis=1))) ** 2)
    spec -= spec.max()

    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / rate))
    if center:
        f = f + center
    t = (idx[:, 0] + nfft / 2) / rate

    plt.figure(figsize=(11, 6))
    plt.imshow(spec, aspect="auto", origin="lower", cmap="turbo",
               extent=(f[0] / 1e6, f[-1] / 1e6, t[0], t[-1]),
               vmin=-80, vmax=0, interpolation="nearest")
    plt.colorbar(label="dB rel max")
    plt.xlabel("Frequency [MHz]" if center else "Offset [MHz]")
    plt.ylabel("Time [s]")
    plt.title(f"Waterfall: {path}")
    plt.tight_layout()
    if out:
        plt.savefig(out, dpi=130)
        print(f"wrote {out}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
