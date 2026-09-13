#!/usr/bin/env python3
"""Audio hiss/junk metric for demodulator output comparison.

Measures the ratio of audio energy above/below a split frequency
(default 3.5 kHz) over the first N seconds of each input WAV, prints a
table and saves a bar+PSD plot.  Designed for A/B of demodulator
variants against a reference (e.g. rxx_multi output): the above-band
shelf is wideband noise through the discriminator; comb structure is
periodic seam clicks.

Usage:
  audio_hiss_metric.py OUT.png REF.wav CAND.wav [CAND.wav ...]
      [--seconds S] [--split HZ] [--lp HZ]

  REF.wav   reference recording (plotted gray, first bar)
  CAND.wav  candidate recordings
  --seconds S  analyze only the first S seconds (default 7)
  --split HZ   band split frequency (default 3500)
  --lp HZ      apply a 129-tap lowpass at HZ to candidates only
               (default 8000; skipped for signals whose Nyquist is
               below it; 0 = off)
"""
import sys
import numpy as np
import wave
from scipy.signal import welch, firwin, lfilter
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

COLORS = ["gray", "tab:cyan", "tab:blue", "tab:red", "tab:orange",
          "tab:green", "tab:purple", "tab:brown"]


def load_wav(path, seconds):
    with wave.open(path) as w:
        fr = w.getframerate()
        n = int(min(seconds * fr, w.getnframes()))
        a = np.frombuffer(w.readframes(n), dtype=np.int16).astype(float)
    return a, fr


def main():
    import argparse
    ap = argparse.ArgumentParser(
        description="Hi/lo audio-band junk metric for demodulator A/B.")
    ap.add_argument("out_png")
    ap.add_argument("ref")
    ap.add_argument("candidates", nargs="+")
    ap.add_argument("--seconds", type=float, default=7.0)
    ap.add_argument("--split", type=float, default=3_500.0)
    ap.add_argument("--lp", type=float, default=8_000.0,
                    help="candidate lowpass Hz (default 8000; skipped "
                         "for signals whose Nyquist is below it; "
                         "0 = off)")
    args = ap.parse_args()

    seconds, split, lp_hz = args.seconds, args.split, args.lp
    ref_path, cand_paths = args.ref, args.candidates
    out_png = args.out_png

    lps = {}
    rows = []
    print(f"{'file':<40} {'0-split':>10} {'split-Nyq':>10} {'hi/lo':>10}")
    for i, path in enumerate([ref_path] + cand_paths):
        label = path.split("/")[-1]
        a, fr = load_wav(path, seconds)
        if i > 0 and lp_hz > 0 and lp_hz < fr / 2:
            if fr not in lps:
                lps[fr] = firwin(129, lp_hz, fs=fr)
            a = lfilter(lps[fr], 1, a)
        f, p = welch(a, fs=fr, nperseg=1024)
        lo = p[(f >= 0) & (f < split)].sum()
        hi = p[(f >= split) & (f <= min(7_000, fr / 2))].sum()
        rows.append((label, f, p / p.max(), lo, hi, hi / lo, i == 0, COLORS[i]))
        print(f"{label:<40} {lo:>10.3e} {hi:>10.3e} {hi/lo:>10.4f}")

    fig, axes = plt.subplots(1, 2, figsize=(15, 5.5))
    ax = axes[0]
    for label, f, p, lo, hi, ratio, is_ref, color in rows:
        ax.semilogy(f, p, lw=1.1, color=color,
                    label=label + (" (ref)" if is_ref else ""))
    ax.axvline(split, color="r", ls="--", lw=1)
    ax.set_xlim(0, 8_000)
    ax.set_xlabel("audio freq [Hz]"); ax.set_ylabel("PSD (norm)")
    ax.set_title(f"Welch PSD, first {seconds:.0f} s")
    ax.legend(fontsize=8); ax.grid(alpha=.3, which="both")

    ax = axes[1]
    names = [r[0] for r in rows]
    vals = [r[5] for r in rows]
    ax.bar(range(len(vals)), vals, color=[r[7] for r in rows])
    ax.set_yscale("log")
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels(names, fontsize=8, rotation=12)
    ax.set_ylabel(f"{split:.0f}-7000 Hz / 0-{split:.0f} Hz energy")
    ax.set_title(f"Out-of-band junk ratio, first {seconds:.0f} s (lower = cleaner)")
    ax.grid(alpha=.3, axis="y", which="both")
    for i, v in enumerate(vals):
        ax.text(i, v * 1.1, f"{v:.4f}", ha="center", fontsize=8)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    print(f"wrote {out_png}")


if __name__ == "__main__":
    main()
