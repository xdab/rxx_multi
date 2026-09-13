#!/usr/bin/env python3
"""Audio content comparison for DSP regression validation.

Compares two demodulator outputs (raw PCM16 or WAV, any combination)
and reports whether they carry the same audio content: bit-exactness,
length agreement, and per-sample difference statistics (max, RMS, SNR).

Intended workflow: run a known-good build over an IQ capture (-I FILE
... -O ref.raw) and save the output as reference; then run the same
session through a modified build into cand.raw and diff them here.
Refactors should come out bit-exact; DSP changes show up as small,
quantified differences instead of silent regressions.

Exit status: 0 = match (bit-exact, or within tolerance), 1 = mismatch,
2 = usage / IO error.

Usage:
  compare_audio.py REF CAND [--max-diff X] [--min-snr DB]
      [--scan-offset N] [--max-seconds S] [--raw-rate HZ]

  REF, CAND      WAV or headerless raw PCM16 (int16 LE mono); format
                 detected by the RIFF header, raw otherwise
  --max-diff X   max |sample difference| (int16 LSB) tolerated
                 (default 0 = bit-exact required by this criterion)
  --min-snr DB   signal-to-difference SNR threshold for a pass
                 (default 60); a file passes if EITHER criterion holds
  --scan-offset N  search offsets -N..+N for the best alignment before
                 comparing (default 0). Use when a DSP change shifts
                 group delay (e.g. a different filter path), which
                 otherwise reads as a full-scale mismatch
  --raw-rate HZ  sample rate of raw files, for reporting only
                 (default 48000)
"""
import sys
import wave

import numpy as np


def load(path, raw_rate):
    """Return (samples as float64, rate, label) for a WAV or raw file."""
    with open(path, "rb") as f:
        magic = f.read(12)
    if magic[:4] == b"RIFF" and magic[8:12] == b"WAVE":
        with wave.open(path) as w:
            rate = w.getframerate()
            channels = w.getnchannels()
            raw = w.readframes(w.getnframes())
        if channels != 1:
            print(f"WARNING: {path} is {channels}-channel; comparing "
                  f"interleaved samples as-is")
        label = f"{path} (wav {rate} Hz"
        label += f", {channels}ch)" if channels != 1 else ")"
    else:
        raw = open(path, "rb").read()
        rate = raw_rate
        label = f"{path} (raw {rate} Hz)"
    if len(raw) % 2:
        raw = raw[:-1]
        print(f"WARNING: {path} has a trailing odd byte; dropped")
    a = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    return a, rate, label


def rms(a):
    return float(np.sqrt(np.mean(a * a))) if len(a) else 0.0


def diff_stats(a, b, off=0):
    """(max abs, rms of difference) over the aligned overlap; off shifts
    the reference relative to the candidate (see best_offset)."""
    if off >= 0:
        n = min(len(a) - off, len(b))
        d = a[off:off + n] - b[:n]
    else:
        n = min(len(a), len(b) + off)
        d = a[:n] - b[-off:-off + n]
    return float(np.max(np.abs(d))) if n else 0.0, rms(d)


def best_offset(a, b, scan):
    """Offset of a relative to b (in -scan..+scan) minimizing mean
    |diff|, evaluated on every 7th sample for speed."""
    n = min(len(a), len(b))
    best_off, best_err = 0, None
    for off in range(-scan, scan + 1):
        lo = max(0, off)
        hi = min(n, n + off)
        if hi - lo < 1000:
            continue
        err = np.mean(np.abs(a[lo:hi:7] - b[lo - off:hi - off:7]))
        if best_err is None or err < best_err:
            best_err, best_off = err, off
    return best_off


def main():
    import argparse
    ap = argparse.ArgumentParser(
        description="Compare two audio files (raw PCM16 / WAV, any "
                    "combo) for content equality; DSP regression gate.")
    ap.add_argument("ref")
    ap.add_argument("cand")
    ap.add_argument("--max-diff", type=float, default=0.0,
                    help="tolerated max |diff| in int16 LSB (default 0)")
    ap.add_argument("--min-snr", type=float, default=60.0,
                    help="SNR pass threshold in dB (default 60)")
    ap.add_argument("--scan-offset", type=int, default=0,
                    help="search +/- N samples for best alignment")
    ap.add_argument("--raw-rate", type=int, default=48_000,
                    help="sample rate of raw files, reporting only")
    ap.add_argument("--max-seconds", type=float, default=None,
                    help="compare only the first N seconds of both files")
    args = ap.parse_args()

    a, rate_a, label_a = load(args.ref, args.raw_rate)
    b, rate_b, label_b = load(args.cand, args.raw_rate)
    if args.max_seconds is not None:
        a = a[:int(rate_a * args.max_seconds)]
        b = b[:int(rate_b * args.max_seconds)]
    if rate_a != rate_b:
        print(f"WARNING: sample rates differ: {rate_a} vs {rate_b}")

    if len(a) != len(b):
        print(f"WARNING: lengths differ: {len(a)} vs {len(b)} samples "
              f"({abs(len(a) - len(b))} = "
              f"{abs(len(a) - len(b)) / max(rate_a, 1):.3f} s)")

    if np.array_equal(a, b):
        print(f"IDENTICAL: {label_a} == {label_b} "
              f"({len(a)} samples, bit-exact)")
        return 0

    off = 0
    if args.scan_offset:
        off = best_offset(a, b, args.scan_offset)
        if off:
            print(f"best alignment: {off} samples "
                  f"({off / rate_b * 1000:.3f} ms)")
    max_diff, diff_rms = diff_stats(a, b, off)
    snr = 20 * np.log10(rms(a) / diff_rms) if diff_rms > 0 else float("inf")

    print(f"{label_a}")
    print(f"{label_b}")
    print(f"  length      : {len(a)} vs {len(b)} samples")
    print(f"  max |diff|  : {max_diff:.0f} LSB")
    print(f"  diff RMS    : {diff_rms:.3f} LSB")
    print(f"  SNR         : {snr:.1f} dB")

    ok = max_diff <= args.max_diff or snr >= args.min_snr
    why = "max-diff" if max_diff <= args.max_diff else "min-snr"
    if ok:
        print(f"MATCH (within tolerance, {why} criterion): "
              f"audio content preserved")
        return 0
    print("MISMATCH: outputs differ beyond tolerance")
    return 1


if __name__ == "__main__":
    sys.exit(main())
