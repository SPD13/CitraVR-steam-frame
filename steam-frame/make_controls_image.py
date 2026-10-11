"""Relabels Valve's Steam Frame controller diagram with CitraVR's default mapping
(docs/images/steam-frame-controls.png, shown in the README's Controls section).

The source is Valve's diagram from the Steamworks documentation:
https://shared.akamai.steamstatic.com/community_assets/images/steamworks_docs/english/steamframe-controllers_1.png
Label boxes are pixel positions in that 2400x1224 image. Needs Pillow and the
Avenir Next font (macOS).

Usage: python make_controls_image.py <source.png> docs/images/steam-frame-controls.png
"""
import sys

from PIL import Image, ImageDraw, ImageFont

SOURCE, OUTPUT = sys.argv[1], sys.argv[2]
FONT_PATH = "/System/Library/Fonts/Avenir Next.ttc"
DEMI_BOLD = 2
font = ImageFont.truetype(FONT_PATH, 30, index=DEMI_BOLD)
title_font = ImageFont.truetype(FONT_PATH, 40, index=DEMI_BOLD)
small_font = ImageFont.truetype(FONT_PATH, 26, index=DEMI_BOLD)
WHITE = (255, 255, 255, 255)
ACCENT = (140, 200, 255, 255)  # CitraVR-specific notes
LINE_HEIGHT = 36
ARROW = "→"
ARROW_WIDTH = 30

image = Image.open(SOURCE).convert("RGBA")
draw = ImageDraw.Draw(image)


def erase(box, margin=8):
    x0, y0, x1, y1 = box
    draw.rectangle((x0 - margin, y0 - margin, x1 + margin, y1 + margin), fill=(0, 0, 0, 255))


def erase_line_keeping_outlines(x0, x1, y0, y1):
    """Erases a horizontal line in the band, keeping the outlines that cross it:
    each column is redrawn by blending what is just above and below the band."""
    pixels = image.load()
    for x in range(x0, x1 + 1):
        above, below = pixels[x, y0 - 2], pixels[x, y1 + 2]
        for y in range(y0 - 1, y1 + 2):
            t = (y - (y0 - 2)) / ((y1 + 2) - (y0 - 2))
            pixels[x, y] = tuple(round(a + (b - a) * t) for a, b in zip(above, below))


def text_width(text, f):
    width = 0
    for i, part in enumerate(text.split(ARROW)):
        if i > 0:
            width += ARROW_WIDTH
        width += f.getlength(part)
    return width


def draw_text(x, y, text, f, fill):
    """Draws text at (x, y) (top left), with arrows drawn as graphics."""
    for i, part in enumerate(text.split(ARROW)):
        if i > 0:
            mid = y + f.size * 0.62
            draw.line((x + 5, mid, x + ARROW_WIDTH - 8, mid), fill=fill, width=3)
            draw.polygon(
                [(x + ARROW_WIDTH - 4, mid), (x + ARROW_WIDTH - 12, mid - 6),
                 (x + ARROW_WIDTH - 12, mid + 6)], fill=fill)
            x += ARROW_WIDTH
        draw.text((x, y), part, font=f, fill=fill)
        x += f.getlength(part)


def label(box, lines, align="center", valign="center", f=font, fill=WHITE, line_height=LINE_HEIGHT):
    """Replaces the label in `box` with `lines`, aligned like the original."""
    erase(box)
    x0, y0, x1, y1 = box
    height = line_height * len(lines)
    if valign == "bottom":
        top = y1 - height + 6
    else:
        top = (y0 + y1) / 2 - height / 2 - 4
    for i, line in enumerate(lines):
        width = text_width(line, f)
        if align == "left":
            x = x0
        elif align == "right":
            x = x1 - width
        else:
            x = (x0 + x1) / 2 - width / 2
        draw_text(x, top + i * line_height, line, f, fill)


# Left controller (front).
label((410, 202, 562, 222), ["Circle Pad", "click: Select"], valign="bottom")
label((399, 315, 461, 340), ["Home"], valign="bottom")
label((107, 474, 182, 500), ["D-pad"], align="right")
label((95, 568, 179, 616), ["Steam", "(SteamVR)"], align="right")
# Grip buttons (both controllers).
label((576, 671, 752, 727), ["Grips:", "not mapped"])
# Right controller (front).
label((758, 206, 910, 226), ["C-Stick", "click: Start"], valign="bottom")
label((862, 317, 930, 340), ["Show/hide", "menu bar"], align="left", valign="bottom")
label((1141, 481, 1212, 499), ["A→B   B→A", "X→Y   Y→X"], align="left")
label((1141, 573, 1225, 621), ["Steam", "(SteamVR)"], align="left")
# Right controller (seen from the front, so mirrored: the third drawing).
label((1595, 222, 1694, 246), ["Bumper: R"], valign="bottom")
label((1471, 292, 1610, 348), ["Trigger:", "ZR or click"])
# Left controller (seen from the front: the fourth drawing).
label((1975, 222, 2074, 246), ["Bumper: L"], valign="bottom")
label((2055, 292, 2194, 348), ["Trigger:", "ZL or click"])
# "Capacitive finger tracking" isn't part of the mapping: remove it and its leader
# lines, keeping the controller outlines they cross.
erase((1728, 796, 1937, 852))
for band in ((1548, 1760), (1908, 2120)):
    erase_line_keeping_outlines(band[0], band[1], 804, 820)
# The trigger rule, under the drawings of the triggers.
label((1440, 1080, 2230, 1150),
      ["Triggers click when pointing at a panel (bottom screen, menus),",
       "and press ZL / ZR when pointing anywhere else"],
      f=small_font, fill=ACCENT, line_height=36)

# Title.
title = "CitraVR on Steam Frame: default controls"
draw.text(((image.width - title_font.getlength(title)) / 2, 50), title, font=title_font, fill=WHITE)

image.save(OUTPUT, optimize=True)
print("saved", OUTPUT, image.size)
