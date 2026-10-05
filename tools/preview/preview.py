#!/usr/bin/env python3
"""Preview the display on your computer, using the firmware's own scene code.

    pip install pillow
    python3 tools/preview/preview.py                       # built-in test covers + idle clock
    python3 tools/preview/preview.py --cover album.jpg --title "Song" --artist "Artist"
    python3 tools/preview/preview.py --gif                 # animated, a few seconds each

PNGs land in tools/preview/out/. Needs clang++.
"""
import argparse
import os
import re
import subprocess
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(HERE, "build")
OUT = os.path.join(HERE, "out")
PIXEL_ASPECT = 1.273  # include/config.h DISPLAY_PIXEL_ASPECT
SCALE = 3


def build():
    os.makedirs(BUILD, exist_ok=True)
    exe = os.path.join(BUILD, "preview")
    render = os.path.join(ROOT, "src", "render")
    sources = [os.path.join(HERE, "preview.cpp")] + [
        os.path.join(render, f) for f in sorted(os.listdir(render)) if f.endswith(".cpp")]
    cmd = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall",
           "-I", os.path.join(ROOT, "include"), "-I", os.path.join(ROOT, "src"),
           "-o", exe] + sources
    subprocess.run(cmd, check=True)
    return exe


def render(exe, args, ms):
    """Run the renderer for one frame; returns (image as the monitor shows it, stdout)."""
    ppm = os.path.join(BUILD, "frame.ppm")
    r = subprocess.run([exe, ppm] + args + [str(ms)], capture_output=True, text=True, check=True)
    img = Image.open(ppm)
    img.load()
    w, h = img.size
    # Scale up, stretching horizontally by the real pixel aspect.
    tall = img.resize((w, h * SCALE), Image.NEAREST)
    return tall.resize((round(w * PIXEL_ASPECT * SCALE), h * SCALE), Image.BILINEAR), r.stdout


def save(exe, args, name, ms, gif):
    if not gif:
        img, out = render(exe, args, ms)
        path = os.path.join(OUT, name + ".png")
        img.save(path)
        print(f"{path}  {out.strip()}")
        return
    # 25 fps for 6 s: long enough to see a title scroll start.
    frames = [render(exe, args, ms + i * 40)[0].resize((489, 360), Image.LANCZOS)
              for i in range(150)]
    path = os.path.join(OUT, name + ".gif")
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=40, loop=0)
    print(path)


SAVERS = ["waves", "sphere", "vinyl", "ridges", "vfd", "wired", "haze", "network", "tunnel", "scope",
          "terminal", "static", "navi", "redsky", "psyche", "crossing"]  # src/render/savers.h order


def decoded_cover(path, raw):
    # Same power-of-two reduction as the firmware.
    art_h = int(re.search(r"ART_H = (\d+)", open(os.path.join(ROOT, "src", "render", "art.h")).read()).group(1))
    size = 640
    while size // 2 >= art_h:
        size //= 2
    img = Image.open(path).convert("RGB").resize((640, 640), Image.LANCZOS)
    img = img.resize((size, size), Image.BOX)
    with open(raw, "wb") as f:
        f.write(img.tobytes())
    return img.size


def test_covers():
    """Synthetic stand-ins for album art: a warm one, a cool one, a mono one."""
    covers = []
    os.makedirs(BUILD, exist_ok=True)

    img = Image.new("RGB", (300, 300))
    d = ImageDraw.Draw(img)
    for y in range(300):
        d.line([(0, y), (300, y)], fill=(235 - y // 4, 120 - y // 6, 50 + y // 10))
    d.ellipse([70, 60, 230, 220], fill=(250, 210, 90))
    d.rectangle([0, 230, 300, 300], fill=(60, 25, 30))
    p = os.path.join(BUILD, "cover_warm.png")
    img.save(p)
    covers.append((p, "Hora Dorada", "Los Mangles & Ana Tijoux"))

    img = Image.new("RGB", (300, 300), (20, 40, 90))
    d = ImageDraw.Draw(img)
    for i in range(8):
        d.rectangle([20 + i * 14, 20 + i * 12, 280 - i * 12, 140 + i * 12],
                    outline=(60 + i * 20, 160 + i * 10, 230), width=3)
    d.ellipse([150, 160, 280, 290], fill=(30, 190, 200))
    p = os.path.join(BUILD, "cover_cool.png")
    img.save(p)
    covers.append((p, "Midnight Transmission (Extended Club Mix) [Remastered 2026]",
                   "Kölsch, Røyksopp"))

    img = Image.new("RGB", (300, 300), (235, 235, 230))
    d = ImageDraw.Draw(img)
    for i in range(0, 300, 22):
        d.line([(i, 0), (300 - i, 300)], fill=(30, 30, 30), width=4)
    p = os.path.join(BUILD, "cover_mono.png")
    img.save(p)
    covers.append((p, "Canción sin título", "Ñu Quartet"))
    return covers


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cover", help="image file to use as the album art")
    ap.add_argument("--title", default="Song Title")
    ap.add_argument("--artist", default="Artist Name")
    ap.add_argument("--time", default="9:41", help="clock time for the idle frame, HH:MM")
    ap.add_argument("--ms", type=int, default=1200,
                    help="animation time in ms (moves the waves and marquee)")
    ap.add_argument("--gif", action="store_true", help="write animated GIFs instead of PNGs")
    ap.add_argument("--status", help="status line to show under the screensavers")
    args = ap.parse_args()

    exe = build()
    os.makedirs(OUT, exist_ok=True)
    covers = [(args.cover, args.title, args.artist)] if args.cover else test_covers()

    for i, (path, title, artist) in enumerate(covers):
        raw = os.path.join(BUILD, "cover.rgb")
        w, h = decoded_cover(path, raw)
        save(exe, ["playing", raw, str(w), str(h), title, artist], f"playing_{i}",
             args.ms, args.gif)

    hh, mm = args.time.split(":")
    for i, name in enumerate(SAVERS):
        save(exe, ["idle", hh, mm, str(i), args.status or "-"], f"saver_{name}", args.ms, args.gif)


if __name__ == "__main__":
    sys.exit(main())
