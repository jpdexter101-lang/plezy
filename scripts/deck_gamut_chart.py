#!/usr/bin/env python3
"""Known-answer gamut test: does a renderer adapt BT.2020 primaries to the display?

usage: deck_gamut_chart.py gen <out.mp4>      generate, then self-check the encode
       deck_gamut_chart.py verify <clip.mp4>  re-check an existing clip's truth table
       deck_gamut_chart.py check <shot.png> [...]

Every comparison so far could only rank renderers against each other, which
cannot say who is *right*. This chart supplies ground truth.

Each patch is chosen as a colour that BT.709 can already express, then carried
into a BT.2020 / PQ container. Because CIE xy is a property of the colour itself
and not of the RGB space that encodes it, the correct xy on any SDR output is
known exactly and is reachable - no gamut mapping is required or excusable. A
renderer that adapts primaries correctly lands on it; one that treats the
BT.2020 numbers as if they were BT.709 renders every patch undersaturated,
because a given colour needs smaller excursions in the wider space.

Patches sit at 100 nits, below PQ reference white (203), so no tone-mapping
roll-off is involved and luminance scaling cannot be mistaken for a colour
error. Chromaticity is compared rather than absolute RGB precisely because each
renderer is free to choose its own overall brightness.
"""
import os
import subprocess
import sys
import tempfile

W, H = 1920, 1080
COLS, ROWS = 5, 2
PATCH_NITS = 100.0

M1 = 2610 / 16384
M2 = 2523 / 4096 * 128
C1 = 3424 / 4096
C2 = 2413 / 4096 * 32
C3 = 2392 / 4096 * 32

RGB709_TO_XYZ = (
    (0.4124564, 0.3575761, 0.1804375),
    (0.2126729, 0.7151522, 0.0721750),
    (0.0193339, 0.1191920, 0.9503041),
)
XYZ_TO_RGB2020 = (
    (1.7166512, -0.3556708, -0.2533663),
    (-0.6666844, 1.6164812, 0.0157685),
    (0.0176399, -0.0427706, 0.9421031),
)

RGB2020_TO_XYZ = (
    (0.6369580, 0.1446169, 0.1688810),
    (0.2627002, 0.6779981, 0.0593017),
    (0.0000000, 0.0280727, 1.0609851),
)

# All inside BT.709 by construction. The primaries and secondaries sit on the
# gamut edge, where a missing adaptation shows up most strongly; the last three
# are ordinary warm tones of the kind real footage is full of.
PATCHES = [
    ("neutral D65", (1.0, 1.0, 1.0)),
    ("709 red", (1.0, 0.0, 0.0)),
    ("709 green", (0.0, 1.0, 0.0)),
    ("709 blue", (0.0, 0.0, 1.0)),
    ("709 cyan", (0.0, 1.0, 1.0)),
    ("709 magenta", (1.0, 0.0, 1.0)),
    ("709 yellow", (1.0, 1.0, 0.0)),
    ("skin", (0.75, 0.45, 0.35)),
    ("warm orange", (1.0, 0.45, 0.12)),
    ("deep red-orange", (1.0, 0.25, 0.05)),
]


def mul(m, v):
    return tuple(sum(m[r][c] * v[c] for c in range(3)) for r in range(3))


def pq_encode(fraction_of_10000: float) -> float:
    y = max(0.0, min(fraction_of_10000, 1.0))
    return ((C1 + C2 * y**M1) / (1.0 + C3 * y**M1)) ** M2


def pq_decode(signal: float) -> float:
    """PQ signal -> nits, so the chart can report its own real light levels."""
    s = max(0.0, min(signal, 1.0)) ** (1.0 / M2)
    num = max(s - C1, 0.0)
    den = C2 - C3 * s
    return 10000.0 * (num / den) ** (1.0 / M1) if den > 0 else 10000.0


def truth_xy(rgb709):
    x, y, z = mul(RGB709_TO_XYZ, rgb709)
    total = x + y + z
    return x / total, y / total


def encode_patch(rgb709):
    """BT.709 colour -> BT.2020 PQ code values, with every channel below reference white.

    Normalising the brightest *component* to PATCH_NITS rather than the patch's
    luminance is what keeps the test honest. Scaling to a luminance of 100 nits
    would push saturated patches' individual channels far higher - BT.709 red has
    a luminance of only 0.213, so its red channel would land near 295 nits, above
    PQ reference white and back inside the tone-mapping roll-off this chart exists
    to avoid. Uniform scaling leaves chromaticity untouched either way.
    """
    xyz = mul(RGB709_TO_XYZ, rgb709)
    rgb2020 = mul(XYZ_TO_RGB2020, xyz)
    # Clamping negatives only matters for colours outside BT.2020; these are all
    # well inside it, so this never fires and cannot bias the answer.
    rgb2020 = tuple(max(v, 0.0) for v in rgb2020)
    peak = max(rgb2020)
    scale = (PATCH_NITS / 10000.0) / peak
    return tuple(pq_encode(v * scale) for v in rgb2020)


def frame_light_levels():
    """MaxCLL/MaxFALL as this chart actually is, rather than as asserted.

    MaxCLL is the largest light level of any pixel - the maximum RGB component -
    which normalisation pins to PATCH_NITS for every patch. MaxFALL is the frame
    *average luminance*, not the average of those component maxima, so each
    patch's decoded BT.2020 nits go through the luminance row and the results are
    averaged over equal patch areas.
    """
    peaks, lumas = [], []
    for _, rgb in PATCHES:
        nits = [pq_decode(v) for v in encode_patch(rgb)]
        peaks.append(max(nits))
        lumas.append(mul(RGB2020_TO_XYZ, nits)[1])
    return round(max(peaks)), round(sum(lumas) / len(lumas))


def generate(out_path: str) -> int:
    pw, ph = W // COLS, H // ROWS
    rows = []
    for gy in range(ROWS):
        row_pixels = []
        for gx in range(COLS):
            _, rgb709 = PATCHES[gy * COLS + gx]
            codes = bytes()
            for v in encode_patch(rgb709):
                c = max(0, min(65535, round(v * 65535)))
                codes += bytes((c >> 8, c & 0xFF))  # PPM 16-bit is big-endian
            row_pixels.append(codes * pw)
        rows.append(b"".join(row_pixels) * ph)
    body = b"".join(rows)

    with tempfile.NamedTemporaryFile(suffix=".ppm", delete=False) as tmp:
        tmp.write(f"P6\n{W} {H}\n65535\n".encode())
        tmp.write(body)
        ppm = tmp.name
    try:
        max_cll, max_fall = frame_light_levels()
        # The declared mastering maximum matches the content's real peak. Declaring
        # the usual 1000 would invite a renderer into its roll-off or peak-detection
        # path on the strength of the metadata alone, which is the one thing this
        # instrument is built to keep out of the measurement.
        mastering = ("G(8500,39850)B(6550,2300)R(35400,14600)WP(15635,16450)"
                     f"L({round(PATCH_NITS * 10000)},1)")
        params = ["hdr-opt=1", "repeat-headers=1", "colorprim=bt2020", "transfer=smpte2084",
                  "colormatrix=bt2020nc", "lossless=1",
                  f"master-display={mastering}",
                  f"max-cll={max_cll},{max_fall}"]
        proc = subprocess.run(
            ["ffmpeg", "-y", "-v", "error", "-loop", "1", "-i", ppm, "-t", "60", "-r", "25",
             "-vf", "setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc,"
                    "format=yuv420p10le",
             "-c:v", "libx265", "-x265-params", ":".join(params),
             "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
             out_path],
            capture_output=True)
    finally:
        os.unlink(ppm)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace")[-2000:])
        return proc.returncode
    print(f"{out_path}: {len(PATCHES)} patches, brightest channel {PATCH_NITS:.0f} nits "
          f"(MaxCLL {max_cll}, MaxFALL {max_fall}), BT.2020/PQ container")
    print(f"{'patch':<18} {'BT.709 linear':<22} {'true xy':<19} {'peak nits':>9}  BT.2020 PQ signal")
    for name, rgb in PATCHES:
        tx, ty = truth_xy(rgb)
        enc = encode_patch(rgb)
        peak = max(pq_decode(v) for v in enc)
        print(f"{name:<18} {str(tuple(round(v, 2) for v in rgb)):<22} "
              f"({tx:.4f},{ty:.4f})   {peak:9.1f}  {tuple(round(v, 4) for v in enc)}")
    return verify(out_path)


def verify(clip: str) -> int:
    """Decode the clip back and confirm the encode did not move the known answer.

    Everything downstream is measured against this chart's truth table, so a bias
    introduced by the PPM -> BT.2020 YUV 4:2:0 conversion would corrupt every
    result while looking like a renderer error. ffmpeg is asked for rgb48le,
    which applies only the inverse matrix and range - no transfer or gamut
    conversion - so the PQ/BT.2020 values written can be recovered directly.
    """
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", clip, "-frames:v", "1", "-f", "rawvideo",
         "-pix_fmt", "rgb48le", "-"],
        capture_output=True, check=True).stdout
    if len(raw) != W * H * 6:
        raise SystemExit(f"{clip}: decoded {len(raw)} bytes, expected {W * H * 6}")

    print(f"\nencode self-check (decoded back through the BT.2020 matrix):")
    print(f"  {'patch':<18} {'true xy':<17} {'recovered xy':<17} {'dxy':>7} {'peak nits':>10}")
    worst = 0.0
    for i, (name, rgb709) in enumerate(PATCHES):
        gx, gy = i % COLS, i // COLS
        cx = round(W * (gx + 0.5) / COLS)
        cy = round(H * (gy + 0.5) / ROWS)
        acc = [0.0, 0.0, 0.0]
        n = 0
        for y in range(cy - 8, cy + 9):
            base = y * W * 6
            for x in range(cx - 8, cx + 9):
                idx = base + x * 6
                for k in range(3):
                    acc[k] += raw[idx + 2 * k] | (raw[idx + 2 * k + 1] << 8)
                n += 1
        signal = [v / n / 65535.0 for v in acc]
        nits = [pq_decode(v) for v in signal]
        xyz = mul(RGB2020_TO_XYZ, nits)
        total = sum(xyz)
        tx, ty = truth_xy(rgb709)
        if total <= 1e-12:
            print(f"  {name:<18} ({tx:.4f},{ty:.4f})   {'(black)':<17}")
            continue
        mx, my = xyz[0] / total, xyz[1] / total
        d = ((mx - tx) ** 2 + (my - ty) ** 2) ** 0.5
        worst = max(worst, d)
        print(f"  {name:<18} ({tx:.4f},{ty:.4f})   ({mx:.4f},{my:.4f})   {d:7.4f} {max(nits):10.1f}")
    verdict = "OK - the chart's truth table survives encoding" if worst < 0.002 else \
        "SUSPECT - encoding itself moved the colours; fix before trusting any capture"
    print(f"  worst dxy {worst:.4f}: {verdict}")
    return 0 if worst < 0.002 else 1


def load_rgb(path):
    dim = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(v) for v in dim.split("x"))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout
    return raw, w, h


def to_linear(code):
    c = code / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def check(paths) -> int:
    loaded = []
    for p in paths:
        raw, w, h = load_rgb(p)
        scale = min(w / W, h / H)
        vw, vh = round(W * scale), round(H * scale)
        loaded.append((p, raw, w, (w - vw) // 2, (h - vh) // 2, vw, vh))

    print(f"{'patch':<18} {'true xy':<17}", end="")
    for p, *_ in loaded:
        print(f" {p.rsplit('/', 1)[-1].replace('.png', '')[-16:]:>24}", end="")
    print()

    totals = {p: [0.0, 0] for p, *_ in loaded}
    for i, (name, rgb709) in enumerate(PATCHES):
        gx, gy = i % COLS, i // COLS
        tx, ty = truth_xy(rgb709)
        print(f"{name:<18} ({tx:.4f},{ty:.4f})", end="")
        for p, raw, w, ox, oy, vw, vh in loaded:
            cx = ox + round(vw * (gx + 0.5) / COLS)
            cy = oy + round(vh * (gy + 0.5) / ROWS)
            acc = [0.0, 0.0, 0.0]
            n = 0
            for y in range(cy - 20, cy + 21, 2):
                base = y * w * 3
                for x in range(cx - 20, cx + 21, 2):
                    idx = base + x * 3
                    for k in range(3):
                        acc[k] += raw[idx + k]
                    n += 1
            lin = [to_linear(v / n) for v in acc]
            xyz = mul(RGB709_TO_XYZ, lin)
            tot = sum(xyz)
            if tot <= 1e-9:
                print(f" {'(black)':>24}", end="")
                continue
            mx, my = xyz[0] / tot, xyz[1] / tot
            d = ((mx - tx) ** 2 + (my - ty) ** 2) ** 0.5
            totals[p][0] += d
            totals[p][1] += 1
            print(f"   {mx:.4f},{my:.4f} d={d:.3f}", end="")
        print()

    print(f"\n{'mean |dxy| from truth':<36}", end="")
    for p, *_ in loaded:
        s, n = totals[p]
        print(f" {s / n:>24.4f}", end="")
    print("\n\n  These patches are inside BT.709, so their true xy is exactly reachable on an SDR")
    print("  output: no gamut mapping is required, and any drift is an error rather than a")
    print("  defensible choice. A renderer that adapts BT.2020 primaries correctly lands near 0;")
    print("  one that treats BT.2020 code values as BT.709 renders them undersaturated, pulling")
    print("  each patch toward the white point.")
    return 0


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    if sys.argv[1] == "gen":
        return generate(sys.argv[2])
    if sys.argv[1] == "check":
        return check(sys.argv[2:])
    if sys.argv[1] == "verify":
        return verify(sys.argv[2])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    raise SystemExit(main())
