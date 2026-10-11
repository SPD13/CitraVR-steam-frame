"""Renders the in-VR controller diagram panel with the default mapping, as the app
lays it out (org.citra.citra_emu.vr.ui.ControllerDiagram), for the README
(docs/images/vr-controller-diagram.png). Keep the labels in sync with
ControllerDiagram.labels().

The panel is 1680x816: 40 px of padding on each side of the 1600x816 drawing
(res/layout/vr_controller_diagram.xml). The app draws its labels in Roboto Bold,
Android's system font; pass a Roboto file (e.g. Roboto[wdth,wght].ttf from Google
Fonts). Needs Pillow, and macOS's Arial Bold for the arrows.

Usage: python make_panel_image.py <vr_controller_diagram.png> <Roboto.ttf> <output.png>
"""
import sys

from PIL import Image, ImageDraw, ImageFont

DRAWING, FONT_FILE, OUTPUT = sys.argv[1:4]
PADDING = 40
SCALE = 1600 / 2400  # label boxes are in pixels of the 2400-pixel source drawing
TEXT_SIZE, NOTE_SIZE, TITLE_SIZE = 25, 22, 32
LABEL_WIDTH, LABEL_HEIGHT, NOTE_WIDTH = 330, 96, 560
LINE_SPACING = 0.95
WHITE = (255, 255, 255)
NOTE_COLOR = (140, 200, 255)
SS = 2  # supersampling
# Roboto from Google Fonts has no arrows; like Android, fall back to another font
# (macOS's Arial Bold).
FALLBACK_FONT = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
ARROW = "→"


def bold(size):
    font = ImageFont.truetype(FONT_FILE, size * SS)
    try:
        font.set_variation_by_axes([700, 100])  # weight, width
    except OSError:
        pass  # a static Roboto Bold file
    return font


# The default Steam Frame mapping, as ControllerDiagram.labels() words it:
# (left, top, right, bottom, align, alignBottom, lines[, color, size, width]).
LABELS = [
    (410, 202, 562, 222, "center", True, ["Circle Pad", "click: Select"]),
    (399, 315, 461, 340, "center", True, ["Home"]),
    (107, 474, 182, 500, "end", False, ["D-pad"]),
    (95, 568, 179, 616, "end", False, ["Steam", "(SteamVR)"]),
    (576, 671, 752, 727, "center", False, ["Grips:", "not mapped"]),
    (758, 206, 910, 226, "center", True, ["C-Stick", "click: Start"]),
    (862, 317, 930, 340, "start", True, ["Show/hide", "menu bar"]),
    (1141, 481, 1212, 499, "start", False, ["A → B   B → A", "X → Y   Y → X"]),
    (1141, 573, 1225, 621, "start", False, ["Steam", "(SteamVR)"]),
    (1595, 222, 1694, 246, "center", True, ["Bumper: R"]),
    (1471, 292, 1610, 348, "center", False, ["Trigger:", "ZR or click"]),
    (1975, 222, 2074, 246, "center", True, ["Bumper: L"]),
    (2055, 292, 2194, 348, "center", False, ["Trigger:", "ZL or click"]),
    (1440, 1080, 2230, 1150, "center", False,
     ["Triggers click when pointing at a panel",
      "(bottom screen, menus), and press their button elsewhere"],
     NOTE_COLOR, NOTE_SIZE, NOTE_WIDTH),
]

drawing = Image.open(DRAWING).convert("RGB")
panel = Image.new("RGB", ((drawing.width + 2 * PADDING) * SS, drawing.height * SS), (0, 0, 0))
panel.paste(drawing.resize((drawing.width * SS, drawing.height * SS), Image.LANCZOS),
            (PADDING * SS, 0))
draw = ImageDraw.Draw(panel)


def fallback(size):
    return ImageFont.truetype(FALLBACK_FONT, size * SS)


def line_width(line, font, size):
    parts = line.split(ARROW)
    return (sum(font.getlength(p) for p in parts)
            + (len(parts) - 1) * fallback(size).getlength(ARROW))


def draw_line(xy, line, font, size, color):
    x, y = xy
    ascent = font.getmetrics()[0]
    for i, part in enumerate(line.split(ARROW)):
        if i > 0:
            arrow_font = fallback(size)
            # Align the arrow's baseline with the text's.
            draw.text((x, y + ascent - arrow_font.getmetrics()[0]), ARROW, font=arrow_font,
                      fill=color)
            x += arrow_font.getlength(ARROW)
        draw.text((x, y), part, font=font, fill=color)
        x += font.getlength(part)


def text_block(lines, font, size, box, h_align, v_align, color):
    """Draws lines in box (supersampled pixels) like a TextView with that gravity."""
    x0, y0, x1, y1 = box
    ascent, descent = font.getmetrics()
    line_height = (ascent + descent) * LINE_SPACING
    height = line_height * (len(lines) - 1) + ascent + descent
    top = y1 - height if v_align == "bottom" else (y0 + y1) / 2 - height / 2
    for i, line in enumerate(lines):
        width = line_width(line, font, size)
        x = {"start": x0, "center": (x0 + x1) / 2 - width / 2, "end": x1 - width}[h_align]
        draw_line((x, top + i * line_height), line, font, size, color)


for label in LABELS:
    left, top, right, bottom, align, align_bottom, lines = label[:7]
    color, size, width = label[7:] if len(label) > 7 else (WHITE, TEXT_SIZE, LABEL_WIDTH)
    left, right, bottom = left * SCALE, right * SCALE, bottom * SCALE
    center_y = (top + label[3]) / 2 * SCALE
    x = {"start": left, "center": (left + right) / 2 - width / 2, "end": right - width}[align]
    y = bottom + 4 - LABEL_HEIGHT if align_bottom else center_y - LABEL_HEIGHT / 2
    box = [(PADDING + x) * SS, y * SS, (PADDING + x + width) * SS, (y + LABEL_HEIGHT) * SS]
    text_block(lines, bold(size), size, box, align, "bottom" if align_bottom else "center", color)

title = "Controller mapping"
title_font = bold(TITLE_SIZE)
draw.text(((PADDING + 800) * SS - title_font.getlength(title) / 2, 24 * SS), title,
          font=title_font, fill=WHITE)

panel = panel.resize((panel.width // SS, panel.height // SS), Image.LANCZOS)
panel.save(OUTPUT, optimize=True)
print("saved", OUTPUT, panel.size)
