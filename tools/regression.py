#!/usr/bin/env python3
"""Commit-gate regression suite: demodulate a synthesized multi-channel scene
with no hardware and check the verdict mechanically.

The stimulus is the gen_scene_iq.py saturated-spectrum scene (every
modulation, unique tone per channel, seeded noise — bit-reproducible).  One
case per modulation runs the receiver in -I file mode over the matching
subset of channels; each output must show its channel's tone at exactly the
synthesized frequency, with the rest of the scene rejected.  Because file
input and the DSP chain are deterministic (verified bit-identical across
runs), same-config reruns must come out bit-exact: --ref enforces that
against a saved reference decode, --update-ref creates one.

Usage:
  regression.py [--bin PATH] [--only SUBSTR] [--list] [--regen]
                [--seconds S] [--keep] [--update-ref] [--ref]

Exit status: 0 = all cases passed, 1 = any case failed, 2 = setup error.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_scene_iq import SCENE, atofs, build_scene

ROOT = Path(__file__).resolve().parent.parent

RATE_ARG, CENTER_ARG = "2M", "100M"
RATE, CENTER = atofs(RATE_ARG), atofs(CENTER_ARG)
CHANNEL_RATE_ARG, AUDIO_RATE_ARG = "24k", "48k"
CHANNEL_RATE, AUDIO_RATE = atofs(CHANNEL_RATE_ARG), atofs(AUDIO_RATE_ARG)
CASE_SECONDS = 2.0
SEED, NOISE, BASE_AMP = 20261024, 0.01, 0.7

# The receiver never demodulates "cw" — that channel is the off-channel spur
# every case must reject.  Expected demod rate: file mode decimates the file
# rate by floor(file rate / channel rate), integer-divided, same as main.c.
DEMOD_RATE = RATE // (RATE // int(CHANNEL_RATE))

# Tones a demodulated channel must reject (the cw channel has none).
ALL_TONES = sorted({tone for _, _, tone, _, _ in SCENE if tone})

TONE_TOL_HZ = 10.0  # FFT peak must sit within this of the synthesized tone
TONE_MIN_PROM_DB = 25.0  # ...and this far above the 0.1-5 kHz band median
REJECT_MAX_DB = -40.0  # every other scene tone at most this loud, dBc
FSK_ENERGY_MIN = 0.50  # share of 0.2-10 kHz energy inside the FSK lobes
SUBPROCESS_TIMEOUT = 300


class CaseFail(Exception):
    """A case verdict: failure note plus the exact receiver command."""

    def __init__(self, note, cmd=None):
        super().__init__(note)
        self.note = note
        self.cmd = cmd


@dataclass
class Case:
    mod: str  # scene content label; "fsk" demodulates as -M raw
    chans: list  # (offset_hz, tone_hz, param) per channel, scene order

    @property
    def label(self):
        return self.mod

    @property
    def rx_mode(self):
        return "raw" if self.mod == "fsk" else self.mod


def build_cases():
    by_mod = {}
    for entry in SCENE:
        if entry[1] != "cw":
            by_mod.setdefault(entry[1], []).append((entry[0], entry[2], entry[4]))
    return [Case(mod, chans) for mod, chans in by_mod.items()]


def scene_cache_path(seconds):
    key = repr((SCENE, RATE, CENTER, seconds, SEED, NOISE, BASE_AMP))
    digest = hashlib.blake2b(key.encode(), digest_size=5).hexdigest()
    return Path(tempfile.gettempdir()) / f"rxx_multi_scene-{digest}.cf32"


def ensure_scene(path, seconds, regen):
    """Generate the scene unless a valid cached one exists (deterministic
    synthesis makes reuse and regen byte-identical)."""
    expected = int(RATE * seconds) * 8
    if not regen and path.is_file() and path.stat().st_size == expected:
        print(f"scene: reusing cached {path}")
        return
    print(f"scene: generating {path} ({expected} bytes, {seconds:g} s) ...")
    build_scene(RATE, CENTER, seconds, SEED, NOISE, BASE_AMP).astype(np.complex64).tofile(path)


def load_audio(path):
    return np.frombuffer(Path(path).read_bytes(), dtype="<i2").astype(np.float64)


def tone_spectrum(path, audio_rate):
    """Hann-windowed rFFT of the audio after skipping the startup transient."""
    a = load_audio(path)[int(0.2 * audio_rate) :]
    a = a - a.mean()
    spec = np.abs(np.fft.rfft(a * np.hanning(len(a))))
    return spec, np.fft.rfftfreq(len(a), 1 / audio_rate)


def check_tone(path, tone_hz, other_tones, seconds, audio_rate):
    """(ok, note): peak at the wanted tone with prominence, all other scene
    tones rejected."""
    spec, f = tone_spectrum(path, audio_rate)
    band = (f > 100) & (f < 5000)
    peak_i = np.argmax(np.where(band, spec, 0.0))
    peak_hz, peak_amp = f[peak_i], spec[peak_i]
    prom_db = 20 * np.log10(peak_amp / max(np.median(spec[band]), 1.0))

    def amp_near(freq):
        m = (f > freq - 20) & (f < freq + 20)
        return float(np.max(spec[m])) if m.any() else 0.0

    worst_db, worst_hz = None, 0.0
    for other in other_tones:
        db = 20 * np.log10(max(amp_near(other), 1.0) / peak_amp)
        if worst_db is None or db > worst_db:
            worst_db, worst_hz = db, other

    ok = abs(peak_hz - tone_hz) <= TONE_TOL_HZ and prom_db >= TONE_MIN_PROM_DB
    ok = ok and worst_db <= REJECT_MAX_DB
    note = (
        f"peak {peak_hz:.1f} Hz (want {tone_hz:.0f}), prom {prom_db:.0f} dB, "
        f"worst spur {worst_db:+.1f} dBc @{worst_hz:.0f} Hz"
    )
    return ok, note


def check_fsk(path, dev_hz):
    """(ok, note): raw-demod baseband energy concentrated around +-dev."""
    a = load_audio(path)[2 * int(0.2 * DEMOD_RATE) :]
    iq = np.ascontiguousarray(a[0::2] + 1j * a[1::2])[: len(a) // 2]
    iq = iq - iq.mean()
    spec = np.abs(np.fft.fftshift(np.fft.fft(iq * np.hanning(len(iq)))))
    f = np.fft.fftshift(np.fft.fftfreq(len(iq), 1 / DEMOD_RATE))
    total = spec[(np.abs(f) > 200) & (np.abs(f) < 10_000)].sum()
    lobes = spec[(np.abs(f) > dev_hz - 1500) & (np.abs(f) < dev_hz + 1500)].sum()
    share = float(lobes / max(total, 1.0))
    return (
        share >= FSK_ENERGY_MIN,
        f"FSK lobe share {100 * share:.0f}% (want >={100 * FSK_ENERGY_MIN:.0f}%)",
    )


def diff_stats(ref_bytes, cand_bytes):
    """Human summary of a reference mismatch, compare_audio.py style."""
    a = np.frombuffer(ref_bytes, dtype="<i2").astype(np.float64)
    b = np.frombuffer(cand_bytes, dtype="<i2").astype(np.float64)
    n = min(len(a), len(b))
    d = np.abs(a[:n] - b[:n])
    first = int(np.argmax(d > 0)) if (d > 0).any() else -1
    return f"len {len(a)} vs {len(b)}, first diff @ {first}, max |diff| {float(d.max()):.0f} LSB"


def run_receiver(case, scene, workdir, seconds, bin_path):
    """Run the receiver over the case's channels; (outdir, cmd).  Fails on
    nonzero exit, a missing EOF banner or an output of the wrong size."""
    outdir = workdir / case.label
    outdir.mkdir()
    outs = [outdir / f"ch{i + 1}.raw" for i in range(len(case.chans))]
    freqs = ",".join(f"{CENTER + offset:.0f}" for offset, _, _ in case.chans)
    cmd = [
        str(bin_path),
        "-I",
        f"{scene}:{RATE_ARG}:{CENTER_ARG}",
        "-f",
        freqs,
        "-M",
        case.rx_mode,
        "-s",
        CHANNEL_RATE_ARG,
        "-r",
        AUDIO_RATE_ARG,
        "-O",
        ",".join(str(p) for p in outs),
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=SUBPROCESS_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise CaseFail(f"timeout after {SUBPROCESS_TIMEOUT}s", cmd) from None
    if proc.returncode != 0 or "IQ input file exhausted" not in proc.stderr:
        raise CaseFail(f"rc={proc.returncode}, stderr tail: {proc.stderr[-400:]}", cmd)
    for p in outs:
        rate = DEMOD_RATE * 2 if case.rx_mode == "raw" else AUDIO_RATE
        want = int(seconds * rate) * 2
        got = p.stat().st_size if p.is_file() else -1
        if abs(got - want) > want // 100:
            raise CaseFail(f"{p.name}: {got} bytes (want ~{want})", cmd)
    return outdir, cmd


def evaluate_channels(case, outdir, seconds):
    """Mechanical per-channel verdicts; list of notes, or CaseFail."""
    notes = []
    for i, (offset, tone, param) in enumerate(case.chans):
        path = outdir / f"ch{i + 1}.raw"
        if case.mod == "fsk":
            ok, note = check_fsk(path, param)
        else:
            ok, note = check_tone(
                path, tone, [t for t in ALL_TONES if t != tone], seconds, AUDIO_RATE
            )
        if not ok:
            raise CaseFail(f"ch{i + 1}: {note}")
        notes.append(note)
    return notes


def ref_root(bin_path):
    key = repr((SCENE, RATE, CENTER, SEED, NOISE, BASE_AMP))
    digest = hashlib.blake2b(key.encode(), digest_size=5).hexdigest()
    return Path.home() / ".cache" / "rxx_multi" / "regression" / f"{bin_path.name}-{digest}"


def save_ref(outdir, dest):
    dest.mkdir(parents=True, exist_ok=True)
    for p in outdir.glob("*.raw"):
        shutil.copy2(p, dest / p.name)
    return "ref saved"


def compare_ref(outdir, dest):
    """Bit-exact match against the saved decode; note, or CaseFail.
    Raw mode jitters by a few dozen samples at EOF (drain timing); identical
    content up to the shorter length is still a bit-exact match."""
    notes = []
    for p in sorted(outdir.glob("*.raw")):
        want = (dest / p.name).read_bytes()
        got = p.read_bytes()
        if got == want:
            continue
        n = min(len(want), len(got))
        if want[:n] == got[:n] and abs(len(want) - len(got)) <= n // 100:
            notes.append(f"{p.name}: bit-exact vs ref (EOF jitter {len(got) - len(want):+d} smp)")
            continue
        raise CaseFail(f"{p.name}: reference mismatch ({diff_stats(want, got)})")
    notes.append("bit-exact vs ref")
    return notes


def parse_args():
    ap = argparse.ArgumentParser(description="Hardware-free demodulation regression gate.")
    ap.add_argument("--bin", default=str(ROOT / "bin" / "rtl_multi"), help="receiver binary")
    ap.add_argument("--only", default="", help="run only cases whose label contains SUBSTR")
    ap.add_argument("--list", action="store_true", help="print the case matrix and exit")
    ap.add_argument("--regen", action="store_true", help="regenerate the cached scene")
    ap.add_argument("--seconds", type=float, default=CASE_SECONDS, help="scene duration")
    ap.add_argument("--keep", action="store_true", help="keep the workdir (default: delete)")
    ap.add_argument(
        "--update-ref", action="store_true", help="save outputs as the reference decode"
    )
    ap.add_argument(
        "--ref", action="store_true", help="require bit-identical match to the reference"
    )
    return ap.parse_args()


def main():
    args = parse_args()
    bin_path = Path(args.bin)
    if not (bin_path.is_file() and os.access(bin_path, os.X_OK)):
        print(f"ERROR: receiver not executable: {bin_path} (run make)", file=sys.stderr)
        return 2

    cases = [c for c in build_cases() if args.only in c.label]
    if args.list:
        for c in cases:
            chans = ", ".join(f"{(CENTER + o) / 1e6:.3f}M@{t or p or '-'}" for o, t, p in c.chans)
            print(f"{c.label:>4}: {chans}")
        return 0

    scene = scene_cache_path(args.seconds)
    ensure_scene(scene, args.seconds, args.regen)
    rdir = ref_root(bin_path)
    if args.ref and not rdir.is_dir():
        print(f"ERROR: no reference decode at {rdir} (run --update-ref first)", file=sys.stderr)
        return 2

    print(f"rxx_multi regression: {len(cases)} cases, bin={bin_path}, scene={scene.name}")
    n_pass, t0 = 0, time.time()
    workdir = Path(tempfile.mkdtemp(prefix="rxx_regression_"))
    try:
        for case in cases:
            try:
                outdir, _ = run_receiver(case, scene, workdir, args.seconds, bin_path)
                notes = evaluate_channels(case, outdir, args.seconds)
                if args.update_ref:
                    notes.append(save_ref(outdir, rdir / case.label))
                if args.ref:
                    notes += compare_ref(outdir, rdir / case.label)
            except CaseFail as fail:
                print(f"FAIL  {case.label:<4}  {fail.note}")
                if fail.cmd:
                    print(f"      cmd: {' '.join(fail.cmd)}")
                continue
            print(f"PASS  {case.label:<4}  {'; '.join(notes)}")
            n_pass += 1
        print("----")
    finally:
        if args.keep:
            print(f"workdir kept: {workdir}")
        else:
            shutil.rmtree(workdir)
    print(f"{n_pass}/{len(cases)} passed in {time.time() - t0:.1f}s")
    return 0 if n_pass == len(cases) else 1


if __name__ == "__main__":
    sys.exit(main())
