"""Draws the Show Control app icon (AppIcon.icns): one magenta light over the room, its beam
fading to the floor, on Sentia ink.

    .venv/bin/python tools/showcontrol/make_icon.py   (needs pillow; macOS's iconutil)
"""
import os
import subprocess
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
S = 1024
SCALE = 4  # drawn large, then scaled down for smooth edges
INK = (0x17, 0x1D, 0x3A, 255)
MAGENTA = (255, 0, 255, 255)


def beam():
    W = S * SCALE
    s = lambda v: int(v * SCALE)
    tile = Image.new("L", (W, W), 0)
    ImageDraw.Draw(tile).rounded_rectangle([s(100), s(100), s(924), s(924)], s(185), fill=255)
    img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    img.paste(INK, (0, 0), tile)

    # The beam: widening downward, fading out by the floor, cut to the tile.
    cone = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(cone)
    top, bottom = s(400), s(924)
    for y in range(top, bottom):
        t = (y - top) / (bottom - top)
        half = s(70) + t * s(260)
        d.line([(s(512) - half, y), (s(512) + half, y)], fill=int(150 * (1 - t) ** 1.6))
    img.paste(MAGENTA, (0, 0), ImageChops.multiply(cone, tile))

    # The light, with a soft glow around it.
    light = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    ImageDraw.Draw(light).ellipse([s(412), s(240), s(612), s(440)], fill=MAGENTA)
    glow = light.filter(ImageFilter.GaussianBlur(s(40)))
    glow.putalpha(ImageChops.multiply(glow.getchannel("A"), Image.new("L", (W, W), 120)))
    img = Image.alpha_composite(Image.alpha_composite(img, light), glow)
    return img.resize((S, S), Image.LANCZOS)


def main():
    big = beam()
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            big.resize((size, size), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}.png"))
            big.resize((size * 2, size * 2), Image.LANCZOS).save(os.path.join(iconset, f"icon_{size}x{size}@2x.png"))
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", os.path.join(HERE, "AppIcon.icns")], check=True)
    big.save(os.path.join(HERE, "AppIcon.png"))
    print("Wrote tools/showcontrol/AppIcon.icns")


if __name__ == "__main__":
    main()
