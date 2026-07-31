#!/usr/bin/env python3
"""Measure the tone-mapping curve a player applied, from a screenshot of a PQ chart.

usage: deck_sample_chart.py <shot.png> <peak_nits> [label]

The ramp clips are static charts of known PQ code values on *neutral* chroma, so
a screenshot of an SDR output is a direct measurement: for every patch the input
luminance in nits is known exactly, and the captured sRGB value is what the eye
saw. That turns "which roll-off looks better" into a curve comparison.

Neutral chroma is also the limit of what this measures: it says nothing about
BT.2020 -> sRGB gamut mapping, saturation or hue. Those need the colour chart
and the real-world clip, judged separately.

No third-party imaging library is available on the Deck, so pixels come from
ffmpeg as raw rgb24 and are averaged in plain Python over small patches.
"""
import subprocess
import sys

# SMPTE ST 2084, same constants as the generator.
M1 = 2610 / 16384
M2 = 2523 / 4096 * 128
C1 = 3424 / 4096
C2 = 2413 / 4096 * 32
C3 = 2392 / 4096 * 32

SRC_W, SRC_H = 1920, 1080
BANDS = 4


def pq_from_nits(nits: float) -> float:
    y = max(0.0, min(nits / 10000.0, 1.0))
    num = C1 + C2 * (y**M1)
    den = 1.0 + C3 * (y**M1)
    return (num / den) ** M2


def nits_from_pq(signal: float) -> float:
    """Inverse PQ, so a position along the ramp band maps back to nits."""
    s = max(0.0, min(signal, 1.0)) ** (1.0 / M2)
    num = max(s - C1, 0.0)
    den = C2 - C3 * s
    if den <= 0:
        return 10000.0
    return 10000.0 * (num / den) ** (1.0 / M1)


def load_rgb(path: str):
    dim = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(v) for v in dim.split("x"))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    if len(raw) != w * h * 3:
        raise SystemExit(f"{path}: expected {w * h * 3} bytes, got {len(raw)}")
    return raw, w, h


def patch(raw, w, cx, cy, half=6):
    """Mean R,G,B over a small square. Small enough to stay inside a patch even
    if the window geometry is off by a few pixels, large enough to average out
    the dither both mpv and KWin apply when reducing to 8 bits."""
    r = g = b = n = 0
    for y in range(cy - half, cy + half + 1):
        base = (y * w) * 3
        for x in range(cx - half, cx + half + 1):
            i = base + x * 3
            r += raw[i]
            g += raw[i + 1]
            b += raw[i + 2]
            n += 1
    return r / n, g / n, b / n


def video_rect(w, h):
    """Where the 16:9 video lands in the window, letterboxed and centred."""
    scale = min(w / SRC_W, h / SRC_H)
    vw, vh = round(SRC_W * scale), round(SRC_H * scale)
    return (w - vw) // 2, (h - vh) // 2, vw, vh


def main() -> int:
    path, peak = sys.argv[1], float(sys.argv[2])
    label = sys.argv[3] if len(sys.argv) > 3 else path

    raw, w, h = load_rgb(path)
    ox, oy, vw, vh = video_rect(w, h)

    steps = [n for n in (1, 5, 20, 100, 203, 400, 700, 1000, 4000, 10000) if n <= peak]
    highs = [n for n in (203, 400, 700, 1000, 4000) if n <= peak]
    if peak not in steps:
        steps.append(peak)
    if peak not in highs:
        highs.append(peak)

    band_h = vh / BANDS

    def band_centre_y(i):
        return oy + round(band_h * (i + 0.5))

    # A blank or wrong-window capture would silently read as "clips everything",
    # so the 203-nit band is checked before any curve is reported.
    white_y = band_centre_y(2)
    wr, wg, wb = patch(raw, w, ox + vw // 2, white_y)

    print(f"=== {label} ===")
    print(f"  capture {w}x{h}  video {vw}x{vh} at {ox},{oy}")
    suspect = "   <-- SUSPECT: near black, is the chart on screen?" if max(wr, wg, wb) < 24 else ""
    print(f"  203-nit reference-white band: R{wr:.0f} G{wg:.0f} B{wb:.0f}{suspect}")
    if oy >= 6:
        lr, lg, lb = patch(raw, w, ox + vw // 2, max(oy // 2, 3))
        print(f"  letterbox: R{lr:.0f} G{lg:.0f} B{lb:.0f}")

    print("  steps band (input nits -> captured sRGB, mean of RGB):")
    prev = None
    for i, nits in enumerate(steps):
        cx = ox + round(vw * (i + 0.5) / len(steps))
        r, g, b = patch(raw, w, cx, band_centre_y(1))
        mean = (r + g + b) / 3
        delta = "" if prev is None else f"  d={mean - prev:+.1f}"
        clip = "  CLIPPED" if mean >= 254.0 else ""
        print(f"    {nits:>6.0f} nits -> {mean:6.1f}  (R{r:.0f} G{g:.0f} B{b:.0f}){delta}{clip}")
        prev = mean

    print("  highlights band:")
    for i, nits in enumerate(highs):
        cx = ox + round(vw * (i + 0.5) / len(highs))
        r, g, b = patch(raw, w, cx, band_centre_y(3))
        print(f"    {nits:>6.0f} nits -> {(r + g + b) / 3:6.1f}")

    # Dense sweep: the ramp band's signal is linear in x, so x maps back to an
    # exact input luminance and the whole curve can be read off one row.
    print("  ramp band sweep:")
    ramp_top = pq_from_nits(peak)
    ry = band_centre_y(0)
    for frac in (0.02, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.98):
        cx = min(max(ox + round((vw - 1) * frac), ox + 7), ox + vw - 8)
        r, g, b = patch(raw, w, cx, ry)
        print(f"    x={frac:4.0%} -> {nits_from_pq(ramp_top * frac):8.1f} nits -> {(r + g + b) / 3:6.1f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
