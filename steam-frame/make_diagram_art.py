"""Draws CitraVR's own diagrams, as original line art made of simple shapes:

  frame-controllers.png  2400x1224  the Steam Frame controllers, front (left and right
                                    controller) and seen from the triggers, with leader
                                    lines and no labels. make_controls_image.py labels it
                                    (README) or scales it to the VR panel's 1600x816.
  3ds-layout.png         1400x760   a New Nintendo 3DS's controls, labelled (VR panel).
  3ds-layout-small.png   700x380    the same, for the README.

The controller features and leader lines sit where the label boxes in
make_controls_image.py and ControllerDiagram.kt expect them (2400x1224 coordinates).
Needs Pillow and macOS's Avenir Next font.

Usage: python make_diagram_art.py OUTPUT_DIR
"""
import math
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

OUT = Path(sys.argv[1])
SS = 3  # supersampling, for smooth lines
LINE = (200, 204, 212)
DIM = (120, 126, 136)
WHITE = (255, 255, 255)
FONT_PATH = "/System/Library/Fonts/Avenir Next.ttc"
DEMI_BOLD = 2


class Canvas:
    """Drawing in final-image coordinates on a supersampled canvas."""

    def __init__(self, width, height):
        self.size = (width, height)
        self.image = Image.new("RGB", (width * SS, height * SS), (0, 0, 0))
        self.draw = ImageDraw.Draw(self.image)

    def _p(self, points):
        return [(x * SS, y * SS) for x, y in points]

    def path(self, points, width=4, color=LINE, closed=False):
        pts = list(points) + ([points[0]] if closed else [])
        self.draw.line(self._p(pts), fill=color, width=round(width * SS), joint="curve")
        r = width * SS / 2
        for x, y in self._p([pts[0], pts[-1]]):  # round caps
            self.draw.ellipse((x - r, y - r, x + r, y + r), fill=color)

    def line(self, a, b, width=3, color=LINE):
        self.path([a, b], width, color)

    def circle(self, center, radius, width=4, color=LINE):
        x, y = center
        self.draw.ellipse(((x - radius) * SS, (y - radius) * SS,
                           (x + radius) * SS, (y + radius) * SS),
                          outline=color, width=round(width * SS))

    def rounded(self, box, radius, width=4, color=LINE):
        x0, y0, x1, y1 = box
        self.draw.rounded_rectangle((x0 * SS, y0 * SS, x1 * SS, y1 * SS), radius * SS,
                                    outline=color, width=round(width * SS))

    def text(self, xy, text, size, color=WHITE, anchor="la"):
        font = ImageFont.truetype(FONT_PATH, size * SS, index=DEMI_BOLD)
        self.draw.text((xy[0] * SS, xy[1] * SS), text, font=font, fill=color, anchor=anchor)

    def save(self, path, width=None):
        w, h = self.size
        image = self.image.resize((w, h), Image.LANCZOS)
        if width is not None:
            image = image.resize((width, round(h * width / w)), Image.LANCZOS)
        image.save(path, optimize=True)
        print("saved", path, image.size)


def bezier(p0, p1, p2, p3, n=40):
    return [tuple((1 - t) ** 3 * a + 3 * (1 - t) ** 2 * t * b + 3 * (1 - t) * t ** 2 * c + t ** 3 * d
                  for a, b, c, d in zip(p0, p1, p2, p3))
            for t in (i / n for i in range(n + 1))]


def ellipse_arc(center, rx, ry, start, end, n=120):
    """Points on an ellipse from angle `start` to `end` (degrees, clockwise from 3 o'clock)."""
    cx, cy = center
    return [(cx + rx * math.cos(math.radians(a)), cy + ry * math.sin(math.radians(a)))
            for a in (start + (end - start) * i / n for i in range(n + 1))]


def controller(c, center, mirror, side_view):
    """A controller's silhouette: a round head and a handle that curves down to a
    lanyard post. `mirror` flips it for the other hand (-1 or 1)."""
    cx, cy = center
    rx, ry = 165, 152

    def m(points):
        return [(cx + mirror * x, cy + y) for x, y in points]

    # Where the handle leaves the head (angles on the head's outline).
    left_angle, right_angle = (118, 62) if not side_view else (112, 66)
    a = ellipse_arc((0, 0), rx, ry, left_angle, left_angle)[0]
    b = ellipse_arc((0, 0), rx, ry, right_angle, right_angle)[0]
    # The head, from the right junction over the top to the left one.
    head = ellipse_arc((0, 0), rx, ry, right_angle, left_angle - 360)
    if side_view:
        # Seen from the triggers, the handle drops more steeply under the head.
        handle = (bezier(a, (a[0] - 18, a[1] + 70), (-88, 330), (-74, 440))
                  + bezier((-74, 440), (-70, 494), (2, 494), (6, 440))[1:]
                  + bezier((6, 440), (16, 350), (30, 250), b)[1:])
    else:
        handle = (bezier(a, (a[0] - 30, a[1] + 60), (-98, 330), (-84, 440))
                  + bezier((-84, 440), (-80, 494), (-8, 494), (-4, 440))[1:]
                  + bezier((-4, 440), (10, 330), (40, 200), b)[1:])
    outline = head + handle[1:]
    c.path(m(outline), closed=True)
    # Lanyard post.
    post_x = cx + mirror * (-34 if side_view else -44)
    c.rounded((post_x - 8, cy + 478, post_x + 8, cy + 506), 8)
    return m(handle)


def front_face(c, center, mirror):
    """The inner bezel of a controller's face."""
    c.path(ellipse_arc(center, 140, 128, 0, 360), width=2.5, color=DIM, closed=True)


def dpad(c, center, arm=56, half=18):
    x, y = center
    r = 6
    pts = [(x - half, y - arm), (x + half, y - arm), (x + half, y - half), (x + arm, y - half),
           (x + arm, y + half), (x + half, y + half), (x + half, y + arm), (x - half, y + arm),
           (x - half, y + half), (x - arm, y + half), (x - arm, y - half), (x - half, y - half)]
    c.path(pts, closed=True)
    c.circle((x, y), 7, width=2.5, color=DIM)
    del r


def stick(c, center):
    c.circle(center, 47)
    c.circle(center, 34, width=3)
    c.circle(center, 6, width=2.5, color=DIM)


def face_button(c, center, letter, radius=22):
    c.circle(center, radius, width=3)
    c.text(center, letter, 22, color=LINE, anchor="mm")


def pill(c, center, w=28, h=12):
    x, y = center
    c.rounded((x - w / 2, y - h / 2, x + w / 2, y + h / 2), h / 2, width=3)


def side_cup(c, center, mirror):
    """Bumper and trigger, seen from the front of the controller."""
    cx, cy = center
    # Bumper: a band across the top of the opening.
    c.rounded((cx - 88, cy - 85, cx + 88, cy - 43), 21)
    # Trigger: a curved paddle under it.
    top = cy - 33
    paddle = (bezier((cx - 88, top), (cx - 92, top + 70), (cx - 40, top + 92), (cx, top + 92))
              + bezier((cx, top + 92), (cx + 40, top + 92), (cx + 92, top + 70), (cx + 88, top))[1:]
              + bezier((cx + 88, top), (cx + 40, top + 14), (cx - 40, top + 14), (cx - 88, top))[1:])
    c.path(paddle, closed=True)
    c.path(bezier((cx - 60, top + 40), (cx - 30, top + 56), (cx + 30, top + 56), (cx + 60, top + 40)),
           width=2.5, color=DIM)
    del mirror


def grip(c, center, height=70):
    x, y = center
    c.rounded((x - 11, y - height / 2, x + 11, y + height / 2), 11, width=3)


def side_grip(c, handle, y, mirror):
    """The grip button, on the outer edge of the handle at height y."""
    side = max if mirror > 0 else min
    near = [p for p in handle if abs(p[1] - y) < 4]
    x = side(p[0] for p in near)
    c.rounded((x - 13, y - 38, x + 13, y + 38), 13, width=3)


def controllers():
    c = Canvas(2400, 1224)
    # Left controller, front.
    controller(c, (430, 515), 1, False)
    front_face(c, (430, 515), 1)
    dpad(c, (358, 488))
    stick(c, (491, 552))
    c.circle((385, 591), 18, width=3)  # Steam button
    pill(c, (436, 440))  # View
    grip(c, (462, 698))
    # Right controller, front.
    controller(c, (902, 515), -1, False)
    front_face(c, (902, 515), -1)
    stick(c, (840, 552))
    face_button(c, (967, 459), "Y")
    face_button(c, (929, 497), "X")
    face_button(c, (1005, 497), "B")
    face_button(c, (967, 535), "A")
    c.circle((930, 591), 18, width=3)  # Steam button
    pill(c, (890, 440))  # Menu
    grip(c, (870, 698))
    # Seen from the triggers: right controller, then left.
    handle = controller(c, (1600, 525), 1, True)
    side_cup(c, (1600, 525), 1)
    side_grip(c, handle, 715, 1)
    handle = controller(c, (2075, 525), -1, True)
    side_cup(c, (2075, 525), -1)
    side_grip(c, handle, 715, -1)
    # Leader lines, to the label boxes of make_controls_image.py / ControllerDiagram.kt.
    for a, b in [((195, 488), (294, 488)), ((195, 591), (362, 591)), ((491, 249), (491, 502)),
                 ((430, 348), (430, 430)), ((483, 691), (567, 691)),
                 ((1032, 490), (1132, 490)), ((958, 591), (1132, 591)), ((840, 249), (840, 503)),
                 ((896, 348), (896, 430)), ((760, 691), (848, 691)),
                 ((1543, 357), (1543, 530)), ((1644, 258), (1644, 459)),
                 ((2025, 258), (2025, 459)), ((2125, 363), (2125, 530))]:
        c.line(a, b, width=2.5)
    c.save(OUT / "frame-controllers.png")
    c.save(OUT / "frame-controllers-1600.png", width=1600)


def labelled(c, text, at, to, size=24, align="right"):
    """A label at `at`, with a leader line from beside it to `to`."""
    font = ImageFont.truetype(FONT_PATH, size, index=DEMI_BOLD)
    width = font.getlength(text)
    x, y = at
    if align == "right":  # text ends at x, line goes right
        c.text((x, y), text, size, anchor="rm")
        c.line((x + 12, y), to, width=2.5)
    elif align == "left":  # text starts at x, line goes left
        c.text((x, y), text, size, anchor="lm")
        c.line((x - 12, y), to, width=2.5)
    else:  # centered under/over, line vertical
        c.text((x, y), text, size, anchor="mm")
        c.line((x, y - 20 if to[1] < y else y + 20), to, width=2.5)
    del width


def three_ds():
    c = Canvas(1400, 760)
    # Upper shell and top screen.
    c.rounded((225, 105, 705, 352), 22)
    c.rounded((312, 130, 618, 330), 10, width=3)
    c.text((465, 230), "3D screen", 22, color=DIM, anchor="mm")
    c.circle((465, 118), 4, width=2.5, color=DIM)  # inner camera
    # Hinge.
    c.rounded((215, 357, 715, 385), 14)
    # Lower shell and touch screen.
    c.rounded((225, 390, 705, 645), 22)
    c.rounded((342, 408, 588, 598), 8, width=3)
    c.text((465, 503), "Touch screen", 22, color=DIM, anchor="mm")
    # Circle Pad and D-pad, left.
    c.circle((283, 450), 34)
    c.circle((283, 450), 23, width=3)
    dpad(c, (283, 548), arm=32, half=11)
    # Face buttons and C-Stick, right.
    c.circle((618, 425), 8, width=3)
    for (x, y), letter in [((648, 455), "X"), ((618, 487), "Y"), ((678, 487), "A"), ((648, 519), "B")]:
        c.circle((x, y), 16, width=3)
        c.text((x, y), letter, 17, color=LINE, anchor="mm")
    # Select and Start (right, under the face buttons), Home (under the touch screen).
    pill(c, (628, 568), 26, 12)
    pill(c, (628, 596), 26, 12)
    pill(c, (465, 620), 40, 14)
    # Labels.
    labelled(c, "Circle Pad", (180, 450), (245, 450))
    labelled(c, "D-Pad", (180, 548), (247, 548))
    labelled(c, "C-Stick", (770, 425), (630, 425), align="left")
    labelled(c, "Face buttons", (770, 487), (698, 487), align="left")
    labelled(c, "Select", (770, 568), (645, 568), align="left")
    labelled(c, "Start", (770, 596), (645, 596), align="left")
    labelled(c, "Home", (465, 690), (465, 631), align="center")
    # Top edge, seen from behind the upper shell's hinge: the shoulder buttons.
    c.text((1055, 85), "Top edge", 24, anchor="mm")
    c.rounded((830, 130, 1280, 190), 16)
    for x0, x1 in [(838, 900), (910, 960), (1150, 1200), (1210, 1272)]:
        c.rounded((x0, 122, x1, 142), 7, width=3)
    for text, x in [("L", 869), ("ZL", 935), ("ZR", 1175), ("R", 1241)]:
        labelled(c, text, (x, 245), (x, 146), align="center")
    c.save(OUT / "3ds-layout.png")
    c.save(OUT / "3ds-layout-small.png", width=700)


OUT.mkdir(parents=True, exist_ok=True)
controllers()
three_ds()
