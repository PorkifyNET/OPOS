#!/usr/bin/env python3
"""Checks that a WAV file contains the given tones, in order.

    check_tones.py FILE HZ [HZ ...]

The file is cut into 100 ms windows. A window counts as a tone when it is
loud and its zero crossings give a frequency within 2% of the expected
one. Each expected tone must appear in at least three windows in a row
(the kernel's "beep" lasts 500 ms), after the previous tone.
"""
import struct
import sys
import wave


def windows(path):
    with wave.open(path) as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        if w.getsampwidth() != 2:
            raise SystemExit("check_tones: expected 16-bit samples")
        data = w.readframes(w.getnframes())
    left = struct.unpack("<%dh" % (len(data) // 2), data)[0::channels]
    size = rate // 10
    for start in range(0, len(left) - size, size):
        block = left[start:start + size]
        peak = max(abs(x) for x in block)
        crossings = sum(1 for a, b in zip(block, block[1:])
                        if (a < 0) != (b < 0))
        yield peak, crossings / 2 / (size / rate)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    expected = [float(x) for x in sys.argv[2:]]
    found = []
    run_frequency, run_length = None, 0
    for peak, frequency in windows(path):
        tone = None
        if peak > 1000:
            for hz in expected:
                if abs(frequency - hz) <= hz * 0.02:
                    tone = hz
        if tone is not None and tone == run_frequency:
            run_length += 1
        else:
            run_frequency, run_length = tone, 1 if tone else 0
        if run_length == 3:
            found.append(tone)
    position = 0
    for tone in found:
        if position < len(expected) and tone == expected[position]:
            position += 1
    if position != len(expected):
        print("check_tones: expected %s, found %s" % (expected, found))
        sys.exit(1)
    print("check_tones: found %s" % ", ".join("%g Hz" % x for x in expected))


if __name__ == "__main__":
    main()
