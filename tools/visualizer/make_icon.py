"""Draws the LMD Visualizer app icon (AppIcon.icns): the LMD-9020, flat, front on, with the
visualizer on its screen (icon_screen.png, drawn by the firmware's own code).

    .venv/bin/python tools/visualizer/make_icon.py   (needs pillow; macOS's iconutil)
"""
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024
SCALE = 4  # drawn large, then scaled down for smooth edges


def monitor():
    W = S * SCALE
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    s = lambda v: int(v * SCALE)

    # The icon grid: a rounded square in warm paper.
    d.rounded_rectangle([s(100), s(100), s(924), s(924)], s(185), fill=(232, 227, 219, 255))

    # Soft shadow under the monitor.
    shadow = Image.new("L", (W, W), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([s(196), s(250), s(828), s(842)], s(40), fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(s(26)))
    img.paste((70, 60, 55, 255), (0, 0), shadow)

    # The body: about 216 x 206 mm, graphite, with a slightly lighter top face.
    bx0, by0, bx1, by1 = s(192), s(214), s(832), s(820)
    d.rounded_rectangle([bx0, by0, bx1, by1], s(34), fill=(48, 49, 53, 255))
    d.rounded_rectangle([bx0, by0, bx1, by0 + s(22)], s(16), fill=(60, 61, 66, 255))

    # The screen, 4:3, recessed in its bezel.
    sx0, sy0 = s(240), s(262)
    sw, sh = s(544), s(408)
    d.rounded_rectangle([sx0 - s(10), sy0 - s(10), sx0 + sw + s(10), sy0 + sh + s(10)], s(10), fill=(28, 29, 32, 255))
    screen = Image.open(os.path.join(HERE, "icon_screen.png")).convert("RGB").resize((sw, sh), Image.LANCZOS)
    img.paste(screen, (sx0, sy0))

    # The control strip: six knobs, then small buttons and the tally lamp.
    ky = s(742)
    for i in range(6):
        kx = s(262) + i * s(62)
        d.ellipse([kx - s(21), ky - s(21), kx + s(21), ky + s(21)], fill=(30, 31, 34, 255), outline=(78, 80, 86, 255), width=s(4))
        d.ellipse([kx - s(4), ky - s(16), kx + s(4), ky - s(8)], fill=(214, 210, 202, 255))
    for row in range(2):
        for col in range(3):
            x = s(658) + col * s(46)
            y = s(722) + row * s(34)
            d.rounded_rectangle([x, y, x + s(34), y + s(20)], s(5), fill=(82, 84, 90, 255))
    d.ellipse([s(788), s(728), s(806), s(746)], fill=(120, 220, 140, 255))

    # The stand under it.
    d.rounded_rectangle([s(250), s(820), s(774), s(848)], s(12), fill=(36, 37, 40, 255))
    return img.resize((S, S), Image.LANCZOS)


def main():
    big = monitor()
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            big.resize((size, size), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}.png"))
            big.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(HERE, "AppIcon.icns")], check=True)
    big.save(os.path.join(HERE, "AppIcon.png"))
    print("Wrote tools/visualizer/AppIcon.icns")


if __name__ == "__main__":
    main()
