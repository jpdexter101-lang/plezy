#!/usr/bin/env python3
"""Freeze one HDR10 frame into a looped clip that carries the source's own metadata.

usage: deck_freeze_hdr_frame.py <in> <timestamp-seconds> <out> [duration]

Comparing three players by screenshot only works if all three are showing the
*same frame*. Plezy (inside a distrobox), mpv and Kodi have very different
startup latencies, so a fixed post-launch sleep lands on different timestamps of
moving footage and the captures would not be comparable at all. Freezing the
chosen frame removes timing from the experiment entirely.

The frame is extracted as raw 10-bit YUV - the decoder's own output, no colour
conversion - and re-encoded losslessly, with the mastering display primaries and
MaxCLL/MaxFALL read back off the source and handed to x265 verbatim. Dropping
that metadata would change the very thing under test, since every tone-mapper
keys on it.
"""
import json
import os
import subprocess
import sys
import tempfile


def probe_side_data(path: str) -> dict:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1",
         "-show_frames", "-of", "json", path],
        capture_output=True, text=True, check=True).stdout
    merged = {}
    for frame in json.loads(out).get("frames", []):
        for side in frame.get("side_data_list", []):
            merged.update({k: v for k, v in side.items() if k != "side_data_type"})
    return merged


def numerator(value, default):
    """ffprobe reports these as fractions whose denominators are exactly the
    units x265 wants (0.00002 for chromaticity, 0.0001 cd/m2 for luminance), so
    the numerator transfers across unchanged."""
    if value is None:
        return default
    return str(value).split("/")[0]


def dimensions(path: str):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    return (int(v) for v in out.split("x"))


def main() -> int:
    src, ts, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    duration = sys.argv[4] if len(sys.argv) > 4 else "60"

    w, h = dimensions(src)
    sd = probe_side_data(src)

    md = "G({},{})B({},{})R({},{})WP({},{})L({},{})".format(
        numerator(sd.get("green_x"), "8500"), numerator(sd.get("green_y"), "39850"),
        numerator(sd.get("blue_x"), "6550"), numerator(sd.get("blue_y"), "2300"),
        numerator(sd.get("red_x"), "35400"), numerator(sd.get("red_y"), "14600"),
        numerator(sd.get("white_point_x"), "15635"), numerator(sd.get("white_point_y"), "16450"),
        numerator(sd.get("max_luminance"), "10000000"), numerator(sd.get("min_luminance"), "1"))

    params = ["hdr-opt=1", "repeat-headers=1", "colorprim=bt2020",
              "transfer=smpte2084", "colormatrix=bt2020nc", f"master-display={md}", "lossless=1"]
    cll = sd.get("max_content")
    fall = sd.get("max_average")
    if cll:
        params.append(f"max-cll={cll},{fall or 0}")

    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", ts, "-i", src, "-frames:v", "1",
         "-f", "rawvideo", "-pix_fmt", "yuv420p10le", "-"],
        capture_output=True, check=True).stdout
    if len(raw) != w * h * 3:  # 4:2:0 10-bit is 3 bytes per pixel
        raise SystemExit(f"{src}: extracted {len(raw)} bytes, expected {w * h * 3}")

    # -stream_loop cannot rewind a pipe, so the frame has to land somewhere
    # seekable before it can be repeated.
    with tempfile.NamedTemporaryFile(suffix=".yuv", delete=False) as tmp:
        tmp.write(raw)
        raw_path = tmp.name
    try:
        proc = subprocess.run(
            ["ffmpeg", "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "yuv420p10le",
             "-s", f"{w}x{h}", "-framerate", "25", "-stream_loop", "-1", "-i", raw_path,
             "-t", duration, "-c:v", "libx265", "-x265-params", ":".join(params),
             "-pix_fmt", "yuv420p10le",
             "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc",
             dst],
            capture_output=True)
    finally:
        os.unlink(raw_path)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode(errors="replace")[-2000:])
        return proc.returncode

    frames = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
         "-show_entries", "stream=nb_read_frames", "-of", "csv=p=0", dst],
        capture_output=True, text=True).stdout.strip()
    if frames.isdigit() and int(frames) < 2:
        raise SystemExit(f"{dst}: only {frames} frame encoded, the loop did not take")

    print(f"{dst}: frozen {src} @ {ts}s  {w}x{h}  {frames} frames  master-display={md}"
          + (f"  max-cll={cll},{fall or 0}" if cll else "  (no CLL in source)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
