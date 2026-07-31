#!/usr/bin/env python3
"""Compare gamut and hue between renderers, pixel for pixel, in linear light.

usage: deck_patch_chroma.py <reference.png> <other.png> [...]

Every capture shows the same frozen frame at the same window geometry, so pixels
correspond exactly and differences are attributable to the renderer.

Two things make a naive colour comparison meaningless, so both are avoided here:

  - "a channel sits at 255" is endpoint occupancy, not evidence of lost detail:
    correct HDR colour bars deliberately contain fully saturated channels. A
    pixel at either endpoint in *any* capture is skipped, because there its true
    value is only bounded, not known.
  - (max-min)/max on gamma-coded values mixes saturation with whatever luminance
    curve the renderer applied. Values are linearised through the inverse sRGB
    EOTF and reduced to CIE xy chromaticity, which is independent of how bright
    the pixel ended up. Luminance is then reported separately, so "dimmer" and
    "different hue" cannot be confused for one another.

Pixels too dark to carry a chromaticity through 8-bit dither are also skipped.
"""
import subprocess
import sys

SRC_W, SRC_H = 1920, 1080

# sRGB (BT.709) primaries, D65 - the space a capture of an SDR output lives in.
RGB_TO_XYZ = (
    (0.4124564, 0.3575761, 0.1804375),
    (0.2126729, 0.7151522, 0.0721750),
    (0.0193339, 0.1191920, 0.9503041),
)


def load_rgb(path: str):
    dim = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(v) for v in dim.split("x"))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    return raw, w, h


def video_rect(w, h):
    scale = min(w / SRC_W, h / SRC_H)
    vw, vh = round(SRC_W * scale), round(SRC_H * scale)
    return (w - vw) // 2, (h - vh) // 2, vw, vh


def to_linear(code: float) -> float:
    """Inverse sRGB EOTF, so ratios between channels mean something physical."""
    c = code / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def chromaticity(mean):
    lin = [to_linear(v) for v in mean]
    xyz = [sum(RGB_TO_XYZ[r][c] * lin[c] for c in range(3)) for r in range(3)]
    total = sum(xyz)
    if total <= 1e-9:
        return None
    return xyz[0] / total, xyz[1] / total, xyz[1]


def main() -> int:
    paths = sys.argv[1:]
    if len(paths) < 2:
        raise SystemExit(__doc__)

    raws = {}
    for p in paths:
        raw, w, h = load_rgb(p)
        raws[p] = raw
    ox, oy, vw, vh = video_rect(w, h)

    # Every capture is the same frozen frame at the same geometry, so pixels
    # already correspond exactly and no flatness proxy is needed to align them.
    # Two exclusions remain, both about whether a pixel can carry the answer:
    # anything at an endpoint in *any* capture (its true value is unknown, only
    # bounded), and anything too dark for chromaticity to survive 8-bit dither.
    ref = paths[0]
    acc = {p: [0.0, 0.0, 0.0, 0] for p in paths[1:]}
    worst = {p: (0.0, None) for p in paths[1:]}
    considered = skipped_endpoint = skipped_dark = 0

    for y in range(oy, oy + vh, 2):
        base = y * w * 3
        for x in range(ox, ox + vw, 2):
            i = base + x * 3
            endpoint = False
            for p in paths:
                raw = raws[p]
                for k in range(3):
                    v = raw[i + k]
                    if v >= 254 or v <= 1:
                        endpoint = True
                        break
                if endpoint:
                    break
            if endpoint:
                skipped_endpoint += 1
                continue
            b = chromaticity([raws[ref][i], raws[ref][i + 1], raws[ref][i + 2]])
            if b is None or b[2] < 0.01:
                skipped_dark += 1
                continue
            considered += 1
            for p in paths[1:]:
                raw = raws[p]
                a = chromaticity([raw[i], raw[i + 1], raw[i + 2]])
                if a is None:
                    continue
                d = ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5
                acc[p][0] += d
                acc[p][1] += (a[2] - b[2]) / b[2]
                acc[p][2] += max(d, 0.0)
                acc[p][3] += 1
                if d > worst[p][0]:
                    worst[p] = (d, (x, y, [raw[i], raw[i + 1], raw[i + 2]],
                                    [raws[ref][i], raws[ref][i + 1], raws[ref][i + 2]]))

    total = considered + skipped_endpoint + skipped_dark
    print(f"video {vw}x{vh}; {considered} of {total} sampled pixels comparable "
          f"({skipped_endpoint} at an endpoint somewhere, {skipped_dark} too dark)")
    if not considered:
        print("  nothing comparable")
        return 0

    print(f"\nchromaticity drift vs {ref.rsplit('/', 1)[-1]} (CIE xy, linear light):")
    print(f"  {'capture':<22} {'mean dxy':>9} {'max dxy':>9} {'mean dY%':>9}   worst pixel")
    for p in paths[1:]:
        tot, dy, _, n = acc[p]
        if not n:
            continue
        loc = ""
        if worst[p][1]:
            x, y, av, bv = worst[p][1]
            loc = f"{x},{y} {av} vs {bv}"
        name = p.rsplit("/", 1)[-1].replace(".png", "")
        print(f"  {name:<22} {tot / n:9.4f} {worst[p][0]:9.4f} {dy / n * 100:+8.1f}%   {loc}")

    print("\n  dxy is Euclidean distance in CIE xy, which is NOT perceptually uniform: the same")
    print("  number means different things in different regions of the diagram, so there is no")
    print("  universal just-noticeable threshold to compare it against. Use it only to rank")
    print("  captures of the same frame against each other - a value an order of magnitude")
    print("  larger than its peers is a real difference in mapping, and the screenshots say")
    print("  whether it matters. dY% is the mean relative change in linear luminance over the")
    print("  pixels that survived filtering, not a whole-image brightness figure; a large dY")
    print("  beside a small dxy means brightness moved rather than hue.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
