#!/usr/bin/env python3
# Convert rtl_multi/rsp_multi raw PCM output (S16_LE, little-endian int16)
# to a WAV file so it can be played with anything.
#
# Usage:
#   raw2wav.py FILE [RATE] [CHANNELS] [OUT.wav]
#
#   FILE      raw PCM input (S16_LE)
#   RATE      sample rate (default 48000, the rtl_multi audio default)
#   CHANNELS  1 = mono (default), 2 = stereo (raw demod mode)
#   OUT.wav   output file (default: FILE with .raw/.dat/none -> .wav)
#
# Example:
#   raw2wav.py audio_173M325.raw
#   raw2wav.py stereo_iq.raw 48000 2 stereo_iq.wav
import os
import sys
import wave


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    rate = int(sys.argv[2]) if len(sys.argv) > 2 else 48000
    ch = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    out = sys.argv[4] if len(sys.argv) > 4 else (
        os.path.splitext(path)[0] + ".wav")

    data = open(path, "rb").read()
    if len(data) % (2 * ch):
        sys.exit(f"file size not a multiple of {2 * ch} bytes (S16_LE x {ch})")

    with wave.open(out, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)

    print(f"{out}: {len(data) // (2 * ch)} frames @ {rate} Hz, "
          f"{'mono' if ch == 1 else f'{ch} channels'}, 16-bit "
          f"({len(data) / (2 * ch) / rate:.1f} s)")


if __name__ == "__main__":
    main()
