#!/usr/bin/env python3
"""Renders the bundled OPOS wallpapers as PNG files.

Usage: python3 tools/mkwallpapers.py OUTPUT_DIR [WIDTH HEIGHT]

Plain Python (zlib only), so it runs anywhere. The pictures are abstract
and drawn from formulas; they are shipped in the initramfs under
/share/wallpapers. Rendering takes some seconds per picture.
"""
import math
import os
import struct
import sys
import zlib


def write_png(path, width, height, rows):
    raw = bytearray()
    previous = bytes(width * 3)
    for row in rows:
        # "Up" filter: smooth pictures compress well with it.
        raw.append(2)
        raw += bytes((row[i] - previous[i]) & 255 for i in range(len(row)))
        previous = row

    def chunk(kind, data):
        body = kind + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", header))
        out.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        out.write(chunk(b"IEND", b""))


def mix(a, b, t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def gradient(stops, t):
    for i in range(len(stops) - 1):
        t0, c0 = stops[i]
        t1, c1 = stops[i + 1]
        if t <= t1:
            return mix(c0, c1, (t - t0) / (t1 - t0) if t1 > t0 else 0)
    return stops[-1][1]


def smooth(t):
    t = 0.0 if t < 0 else 1.0 if t > 1 else t
    return t * t * (3 - 2 * t)


def to_bytes(color, x, y):
    # A 2x2 ordered dither keeps the gradients free of bands.
    bias = (0.0, 0.5, 0.75, 0.25)[(x & 1) | (y & 1) << 1]
    return bytes(max(0, min(255, int(c + bias))) for c in color)


def render(width, height, shade):
    rows = []
    for y in range(height):
        row = bytearray()
        v = y / (height - 1)
        for x in range(width):
            row += to_bytes(shade(x / (width - 1), v, width / height), x, y)
        rows.append(bytes(row))
    return rows


def dusk(u, v, aspect):
    """Layered hills under an evening sky."""
    sky = gradient([(0.0, (18, 22, 58)), (0.45, (74, 52, 120)),
                    (0.72, (214, 112, 98)), (0.85, (246, 186, 120)),
                    (1.0, (246, 186, 120))], v)
    sun = math.exp(-(((u - 0.68) * aspect) ** 2 + (v - 0.70) ** 2) / 0.004)
    color = mix(sky, (255, 236, 190), sun)
    layers = [
        (0.70, 0.050, 3.1, 0.4, (96, 56, 104)),
        (0.77, 0.045, 4.7, 1.9, (66, 40, 86)),
        (0.85, 0.040, 6.3, 3.3, (40, 28, 62)),
        (0.93, 0.030, 8.9, 5.1, (22, 18, 40)),
    ]
    for base, amp, freq, phase, tone in layers:
        ridge = base - amp * (math.sin(u * freq + phase)
                              + 0.5 * math.sin(u * freq * 2.3 + phase * 1.7))
        if v > ridge:
            haze = smooth((v - ridge) / 0.25)
            color = mix(tone, mix(tone, (10, 10, 24), 0.5), haze)
    return color


def waves(u, v, aspect):
    """Soft ocean-coloured ribbons."""
    color = gradient([(0.0, (8, 26, 52)), (0.6, (10, 60, 96)),
                      (1.0, (6, 30, 58))], v)
    for i in range(5):
        centre = 0.30 + 0.11 * i + 0.06 * math.sin(u * (2.2 + i * 0.7) + i)
        width = 0.035 + 0.01 * math.sin(u * 3.0 + i * 2.0)
        band = math.exp(-((v - centre) / width) ** 2)
        tint = gradient([(0.0, (40, 170, 200)), (0.5, (80, 210, 190)),
                         (1.0, (150, 120, 230))],
                        0.5 + 0.5 * math.sin(u * 2.4 + i * 1.7))
        color = mix(color, tint, band * (0.55 - i * 0.06))
    return color


def aurora(u, v, aspect):
    """Northern lights over a dark ridge."""
    color = gradient([(0.0, (4, 8, 24)), (0.7, (10, 24, 48)),
                      (1.0, (14, 30, 52))], v)
    # A few stars, placed by a fixed hash so the picture never changes.
    cell = int(u * 1920) * 7919 + int(v * 1080) * 104729
    if (cell * 2654435761) % 4093 == 0 and v < 0.8:
        color = mix(color, (220, 230, 255), 0.7)
    for i, (hue, bottom) in enumerate([((60, 220, 150), 0.52),
                                        ((90, 200, 230), 0.44),
                                        ((170, 110, 230), 0.36)]):
        # Curtains: brightest along their lower edge, fading upwards.
        edge = bottom + 0.07 * math.sin(u * (3 + i * 1.5) + i * 1.3) \
               + 0.025 * math.sin(u * 11 + i)
        if v < edge:
            glow = math.exp(-(edge - v) / (0.16 + 0.05 * i))
        else:
            glow = math.exp(-(v - edge) / 0.012)
        rays = 0.65 + 0.35 * math.sin(u * 90 + 6 * math.sin(u * 5 + i))
        color = mix(color, hue, 0.55 * glow * rays)
    ridge = 0.86 - 0.04 * math.sin(u * 4.0) - 0.02 * math.sin(u * 11 + 1)
    if v > ridge:
        color = (6, 10, 18)
    return color


def dunes(u, v, aspect):
    """Warm desert dunes in the late light."""
    color = gradient([(0.0, (110, 150, 205)), (0.42, (236, 200, 160)),
                      (0.5, (240, 190, 140)), (1.0, (240, 190, 140))], v)
    for i in range(4):
        base = 0.50 + i * 0.13
        crest = base - 0.05 * math.sin(u * (2.5 + i) + i * 2.1) \
                - 0.02 * math.sin(u * (7 + i * 2) + i)
        if v > crest:
            light = 0.5 + 0.5 * math.sin(u * (2.5 + i) + i * 2.1 + 1.2)
            sand = mix((176, 104, 64), (236, 168, 104), light)
            color = mix(sand, (120, 64, 44), (v - crest) * 2 + i * 0.12)
    return color


PICTURES = [("Dusk", dusk), ("Waves", waves), ("Aurora", aurora),
            ("Dunes", dunes)]


def main():
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    out = sys.argv[1]
    width, height = (int(sys.argv[2]), int(sys.argv[3])) \
        if len(sys.argv) == 4 else (1920, 1080)
    os.makedirs(out, exist_ok=True)
    for name, shade in PICTURES:
        path = os.path.join(out, name + ".png")
        write_png(path, width, height, render(width, height, shade))
        print(path, os.path.getsize(path))


if __name__ == "__main__":
    main()
