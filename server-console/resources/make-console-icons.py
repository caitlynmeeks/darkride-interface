#!/usr/bin/env python3
# Makes the Server Console's icons (its app icon, notification picture and tray icons) from Darkride's mark, the
# master that interface/icon/make-icons.py makes the client's icons from.
#
# usage: python3 make-console-icons.py <mark.png> <this folder>
#   e.g. python3 make-console-icons.py ../../interface/icon/darkride-mark.png .
# needs Pillow.
#
# Written files:
#   console.png, console-beta.png             the app icon picture, 1024 px: navy rounded square, the mark on it
#   console.ico, console-beta.ico             the same, 256 down to 16 px (Windows)
#   console.icns, console-beta.icns           the same (macOS)
#   console-notification.png                  the picture on the console's notifications, 696 px
#   console-tray-win[-stopped|-stopping][@2x].png, console-tray[-stopped].ico
#                                             the Windows tray: the mark on a small navy square; stopped has a red
#                                             cross on it, stopping an amber one, as before
#   console-tray-osx[-stopped|-stopping][@2x].png
#                                             the macOS menu bar: the mark's outline in black (a template image, which
#                                             macOS colours to suit the bar); stopped has a dot, stopping a cross
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

NAVY = (9, 16, 35, 255)        # #091023, the background of darkride.ai's artwork
RED = (255, 85, 85, 255)       # the world server pages' --dr-red
AMBER = (255, 184, 108, 255)   # their --dr-amber

mark_path, out = sys.argv[1:3]
mark = Image.open(mark_path).convert("RGBA")


def save(name, image, **options):
    image.save(os.path.join(out, name), **options)
    print(name, image.size)


def app_icon(size=1024):
    # as the client's (make-icons.py): a rounded square on a transparent margin, a faint shadow, the mark on it
    page = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    margin, radius = round(size * 100 / 1024), round(size * 185 / 1024)
    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (margin, margin + size // 64, size - margin, size - margin + size // 64), radius, fill=(0, 0, 0, 90)
    )
    page.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(size / 48)))
    ImageDraw.Draw(page).rounded_rectangle((margin, margin, size - margin, size - margin), radius, fill=NAVY)
    side = round((size - 2 * margin) * 0.92)
    page.alpha_composite(mark.resize((side, side), Image.LANCZOS), ((size - side) // 2, (size - side) // 2))
    return page


def tray_square(size):
    # a tray icon is tiny: the square fills it (no margin or shadow), and the mark fills the square
    scale = 8
    big = size * scale
    page = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    ImageDraw.Draw(page).rounded_rectangle((0, 0, big - 1, big - 1), round(big * 0.22), fill=NAVY)
    side = round(big * 1.0)
    page.alpha_composite(mark.resize((side, side), Image.LANCZOS), ((big - side) // 2, (big - side) // 2))
    return page.resize((size, size), Image.LANCZOS)


def cross(image, colour, ring=None):
    # a small cross in the bottom right corner, on a ring of the ring colour (or cut out of the picture) so it reads
    size = image.size[0]
    scale = 8
    big = image.resize((size * scale, size * scale), Image.LANCZOS)
    draw = ImageDraw.Draw(big)
    s = size * scale
    box = (s * 0.6, s * 0.6, s * 0.97, s * 0.97)
    pad = s * 0.05
    ring_box = (box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad)
    if ring is None:
        draw.ellipse(ring_box, fill=(0, 0, 0, 0))
    else:
        draw.ellipse(ring_box, fill=ring)
    width = round(s * 0.1)
    draw.line((box[0], box[1], box[2], box[3]), fill=colour, width=width)
    draw.line((box[0], box[3], box[2], box[1]), fill=colour, width=width)
    return big.resize((size, size), Image.LANCZOS)


def dot(image, colour):
    size = image.size[0]
    scale = 8
    big = image.resize((size * scale, size * scale), Image.LANCZOS)
    draw = ImageDraw.Draw(big)
    s = size * scale
    pad = s * 0.05
    box = (s * 0.66, s * 0.66, s * 0.97, s * 0.97)
    draw.ellipse((box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad), fill=(0, 0, 0, 0))
    draw.ellipse(box, fill=colour)
    return big.resize((size, size), Image.LANCZOS)


def template(size):
    # the mark's shape in black, for the macOS menu bar
    side = round(size * 8 * 1.12)
    big = Image.new("RGBA", (size * 8, size * 8), (0, 0, 0, 0))
    # the mark's glow left out: only its solid strokes, or the outline comes out grey
    shape = mark.resize((side, side), Image.LANCZOS).getchannel("A").point(lambda a: 255 if a > 150 else 0)
    black = Image.new("RGBA", (side, side), (0, 0, 0, 255))
    black.putalpha(shape)
    big.alpha_composite(black, ((size * 8 - side) // 2, (size * 8 - side) // 2))
    return big.resize((size, size), Image.LANCZOS)


icon = app_icon()
ico_sizes = [(256, 256), (48, 48), (32, 32), (24, 24), (16, 16)]
for name in ("console", "console-beta"):
    save(name + ".png", icon)
    save(name + ".ico", icon, sizes=ico_sizes)
    save(name + ".icns", icon)
save("console-notification.png", icon.resize((696, 696), Image.LANCZOS))

for suffix, scale in (("", 1), ("@2x", 2)):
    size = 16 * scale
    running = tray_square(size)
    save("console-tray-win" + suffix + ".png", running)
    save("console-tray-win-stopped" + suffix + ".png", cross(running, RED, ring=(255, 255, 255, 255)))
    save("console-tray-win-stopping" + suffix + ".png", cross(running, AMBER, ring=(255, 255, 255, 255)))

    size = 18 * scale
    outline = template(size)
    save("console-tray-osx" + suffix + ".png", outline)
    save("console-tray-osx-stopped" + suffix + ".png", dot(outline, (0, 0, 0, 255)))
    save("console-tray-osx-stopping" + suffix + ".png", cross(outline, (0, 0, 0, 255)))

tray32 = tray_square(32)
save("console-tray.ico", tray32, sizes=[(32, 32), (16, 16)])
save("console-tray-stopped.ico", cross(tray32, RED, ring=(255, 255, 255, 255)), sizes=[(32, 32), (16, 16)])
