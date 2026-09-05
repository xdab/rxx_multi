#!/usr/bin/env python3

import argparse
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
AUDIO_RATE = 48000
BASE_PORT = 8101


def freq_mhz(token):
    token = token.strip()
    mult = 1.0
    last = token[-1].lower() if token else ""
    if last == "k":
        mult, token = 1e3, token[:-1]
    elif last == "m":
        mult, token = 1e6, token[:-1]
    elif last == "g":
        mult, token = 1e9, token[:-1]
    return float(token) * mult / 1e6


def mhz_label(mhz):
    s = f"{mhz:.3f}".rstrip("0")
    if s.endswith("."):
        s += "0"
    return s


def dump_log(log_path):
    data = log_path.read_bytes().decode(errors="replace").strip()
    if data:
        sys.stderr.write(data + "\n")


def main():
    ap = argparse.ArgumentParser(
        description="rsp_multi E2E capture test: three WBFM channels -> TCP, "
        "parallel nc recording, WAV conversion.")
    ap.add_argument("--freqs", default="102.0M,102.4M,103.0M",
                    help="comma-separated channel frequencies")
    ap.add_argument("--duration", type=float, default=15.0,
                    help="seconds to record per channel")
    ap.add_argument("--ramp", type=float, default=4.0,
                    help="seconds to wait for receiver startup")
    ap.add_argument("--out", default="/tmp/opencode",
                    help="output directory for .raw/.wav/.log")
    args = ap.parse_args()

    for tool in ("nc", "ffmpeg"):
        if not shutil.which(tool):
            sys.exit(f"required tool not found: {tool}")

    rx_bin = REPO / "bin" / "rsp_multi"
    if not rx_bin.is_file():
        sys.exit(f"receiver not built: {rx_bin} (run make)")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    freqs = args.freqs.split(",")
    outputs = ",".join(f"tcp:{BASE_PORT + i}" for i in range(len(freqs)))
    log_path = out / "rsp_multi.log"

    with open(log_path, "wb") as log:
        rx = subprocess.Popen(
            [str(rx_bin), "-f", args.freqs, "-M", "wbfm", "-O", outputs],
            stdout=log, stderr=log)

        time.sleep(args.ramp)
        if rx.poll() is not None:
            dump_log(log_path)
            sys.exit("receiver exited during startup")

        recs = []
        for i in range(len(freqs)):
            raw = open(out / f"ch{i + 1}.raw", "wb")
            proc = subprocess.Popen(["nc", "localhost", str(BASE_PORT + i)],
                                    stdout=raw)
            recs.append((proc, raw))

        print(f"recording {len(freqs)} channels for {args.duration:g} s ...")
        time.sleep(args.duration)

        rx.terminate()
        try:
            rx.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rx.kill()

        for proc, raw in recs:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            raw.close()

    if rx.returncode not in (0, -15, -2):
        dump_log(log_path)
        sys.exit(f"receiver exited abnormally (rc={rx.returncode})")

    wavs = []
    for i, token in enumerate(freqs):
        raw = out / f"ch{i + 1}.raw"
        wav = out / f"ch{i + 1}_{mhz_label(freq_mhz(token))}MHz.wav"
        conv = subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "s16le",
             "-ar", str(AUDIO_RATE), "-ac", "1", "-i", str(raw), str(wav)])
        if conv.returncode != 0:
            sys.exit(f"ffmpeg failed on {raw}")
        raw.unlink()
        secs = (wav.stat().st_size - 44) / (2 * AUDIO_RATE)
        wavs.append((wav, freq_mhz(token), secs))
        print(f"ch{i + 1} ({mhz_label(freq_mhz(token))} MHz): "
              f"{wav} ({secs:.1f} s)")

    print("done - wav files ready in " + str(out))


if __name__ == "__main__":
    main()
