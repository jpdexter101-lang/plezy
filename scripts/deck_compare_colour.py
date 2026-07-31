#!/usr/bin/env python3
"""Compare what each renderer did to colour, from captures of one frozen HDR frame.

usage: deck_compare_colour.py <reference.png> <other.png> [...]

Every capture shows the *same* frozen frame at the same geometry, so pixels line
up and differences are attributable to the renderer rather than to timing. This
covers what the neutral PQ ramp cannot: BT.2020 -> sRGB gamut mapping, how much
saturation survives, and whether hue shifts.

Reported per capture:
  clipped   fraction of pixels with a channel at the top of the range - detail
            that has been thrown away and cannot be recovered by the eye
  crushed   fraction with a channel at the bottom, i.e. shadow detail lost
  sat       mean (max-min)/max, a saturation proxy; higher keeps more colour
  chroma p99  how far the most saturated pixels reach
"""
import subprocess
import sys

SRC_W, SRC_H = 1920, 1080
STEP = 2  # subsample; ~230k samples at 1280x720 is far more than enough


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


def stats(raw, w, rect):
    ox, oy, vw, vh = rect
    n = clipped = crushed = 0
    luma = sat = 0.0
    chromas = []
    for y in range(oy, oy + vh, STEP):
        base = y * w * 3
        for x in range(ox, ox + vw, STEP):
            i = base + x * 3
            r, g, b = raw[i], raw[i + 1], raw[i + 2]
            hi, lo = max(r, g, b), min(r, g, b)
            n += 1
            if hi >= 254:
                clipped += 1
            if lo <= 1:
                crushed += 1
            luma += 0.2126 * r + 0.7152 * g + 0.0722 * b
            c = hi - lo
            chromas.append(c)
            if hi:
                sat += c / hi
    chromas.sort()
    return {
        "n": n,
        "clipped": clipped / n,
        "crushed": crushed / n,
        "luma": luma / n,
        "sat": sat / n,
        "chroma_p99": chromas[int(len(chromas) * 0.99)],
        "chroma_mean": sum(chromas) / len(chromas),
    }


def diff(a, b, w, rect):
    ox, oy, vw, vh = rect
    n = 0
    tot = 0.0
    per = [0.0, 0.0, 0.0]
    worst = (0, None)
    for y in range(oy, oy + vh, STEP):
        base = y * w * 3
        for x in range(ox, ox + vw, STEP):
            i = base + x * 3
            d = [a[i + k] - b[i + k] for k in range(3)]
            mag = (abs(d[0]) + abs(d[1]) + abs(d[2])) / 3
            tot += mag
            for k in range(3):
                per[k] += d[k]
            if mag > worst[0]:
                worst = (mag, (x, y, (a[i], a[i + 1], a[i + 2]), (b[i], b[i + 1], b[i + 2])))
            n += 1
    return tot / n, [p / n for p in per], worst


def main() -> int:
    paths = sys.argv[1:]
    if len(paths) < 1:
        raise SystemExit(__doc__)

    ref_raw, w, h = load_rgb(paths[0])
    rect = video_rect(w, h)
    print(f"video rect {rect[2]}x{rect[3]} at {rect[0]},{rect[1]} of {w}x{h}, "
          f"sampling every {STEP} px")
    print(f"{'capture':<22} {'clip%':>7} {'crush%':>7} {'luma':>7} {'sat':>6} "
          f"{'chr.mean':>9} {'chr.p99':>8}")

    loaded = {}
    for p in paths:
        raw = ref_raw if p == paths[0] else load_rgb(p)[0]
        loaded[p] = raw
        s = stats(raw, w, rect)
        name = p.rsplit("/", 1)[-1].replace(".png", "")
        print(f"{name:<22} {s['clipped'] * 100:6.2f}% {s['crushed'] * 100:6.2f}% "
              f"{s['luma']:7.1f} {s['sat']:6.3f} {s['chroma_mean']:9.1f} {s['chroma_p99']:8.0f}")

    if len(paths) > 1:
        print(f"\npixel difference against {paths[0].rsplit('/', 1)[-1]}:")
        for p in paths[1:]:
            mean, per, worst = diff(loaded[p], ref_raw, w, rect)
            name = p.rsplit("/", 1)[-1].replace(".png", "")
            mag, where = worst
            loc = ""
            if where:
                x, y, av, bv = where
                loc = f"   worst at {x},{y}: {av} vs {bv}"
            print(f"  {name:<22} mean|d|={mean:6.2f}  dR={per[0]:+6.2f} "
                  f"dG={per[1]:+6.2f} dB={per[2]:+6.2f}{loc}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
