#!/usr/bin/env python3
"""Verify the compositor's screenshot returns exactly the pixels a client drew.

usage: deck_srgb_probe.py show          fullscreen window of known sRGB patches
       deck_srgb_probe.py check <shot.png>

Every SDR conclusion in this series assumes the screenshot reports what was
presented. That cannot be tested through a video player: a clip tagged BT.709
does not carry sRGB code values, because the two transfer functions differ, so a
player converting BT.709 video for an sRGB display is *supposed* to change the
numbers. Comparing decoded video against the sRGB values it came from measures
the player's transfer handling, not the compositor's - an earlier attempt did
exactly that and read a correct player as a broken capture path.

So the patches are drawn straight into an ordinary GTK surface: no decoder, no
renderer, no transfer conversion anywhere. Whatever the screenshot returns is
then attributable to the compositor and the screenshot path alone.
"""
import subprocess
import sys

# Becomes the Wayland app id, and so the resourceClass the capture harness
# matches on to raise and fullscreen this window. Without it a plain GTK program
# reports itself as "python3" and the harness photographs the desktop instead.
APP_ID = "plezy-srgb-probe"

PATCHES = [(255, 255, 255), (200, 60, 40), (40, 200, 80), (60, 80, 220), (128, 128, 128), (20, 20, 20)]


def on_draw(widget, cr):
    width = widget.get_allocated_width()
    height = widget.get_allocated_height()
    band = width / len(PATCHES)
    for i, (r, g, b) in enumerate(PATCHES):
        # Cairo takes 0..1 device values and applies no colour management, so
        # these land on the surface as the exact 8-bit codes named above.
        cr.set_source_rgb(r / 255.0, g / 255.0, b / 255.0)
        cr.rectangle(i * band, 0, band + 1, height)
        cr.fill()
    # True stops GTK's default handler, which would otherwise paint the theme
    # background over these patches - the whole window then reads as one flat
    # colour and the probe silently measures the theme instead.
    return True


def show() -> int:
    # Imported here so `check` runs on a machine with no display and no GTK.
    import gi

    gi.require_version("Gtk", "3.0")
    from gi.repository import GLib, Gtk

    # Must precede window creation: GTK reads the program name when it builds the
    # toplevel's app id.
    GLib.set_prgname(APP_ID)
    window = Gtk.Window(title=APP_ID)
    area = Gtk.DrawingArea()
    area.connect("draw", on_draw)
    window.add(area)
    window.fullscreen()
    window.set_keep_above(True)
    window.connect("destroy", Gtk.main_quit)
    window.show_all()
    window.present()
    Gtk.main()
    return 0


def check(path: str) -> int:
    dim = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    w, h = (int(v) for v in dim.split("x"))
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, check=True).stdout

    print(f"capture {w}x{h} - plain GTK sRGB surface, no decoder or renderer in the chain")
    print(f"  {'drawn sRGB':<18} {'captured':<18} delta")
    worst = 0
    for i, want in enumerate(PATCHES):
        cx = round(w * (i + 0.5) / len(PATCHES))
        cy = h // 2
        acc = [0, 0, 0]
        n = 0
        for y in range(cy - 10, cy + 11):
            for x in range(cx - 10, cx + 11):
                idx = (y * w + x) * 3
                for k in range(3):
                    acc[k] += raw[idx + k]
                n += 1
        got = tuple(round(a / n) for a in acc)
        d = max(abs(got[k] - want[k]) for k in range(3))
        worst = max(worst, d)
        print(f"  {str(want):<18} {str(got):<18} {d}")

    ok = worst <= 2
    print(f"\n  worst channel delta {worst}: "
          + ("compositor and screenshot path are transparent; the SDR captures are sound"
             if ok else "compositor or screenshot alters pixels - every SDR capture carries that bias"))
    return 0 if ok else 1


def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == "show":
        return show()
    if len(sys.argv) >= 3 and sys.argv[1] == "check":
        return check(sys.argv[2])
    raise SystemExit(__doc__)


if __name__ == "__main__":
    raise SystemExit(main())
