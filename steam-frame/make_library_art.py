#!/usr/bin/env python3
"""Steam library artwork for CitraVR on Steam Frame, from the CitraVR logo
(assets/citravr_logo.png: the lime, "Citra" in black and "VR" in green on white).
The logo is cut out of its white background and "Citra" is turned white, for Steam's
dark library. Output, in steam-frame/library-art/ (installed by install-library-art.py):

    capsule.png  600x900    library grid (portrait capsule)    -> <appid>p.png
    wide.png     920x430    recently played / wide capsule     -> <appid>.png
    hero.png     3840x1240  banner at the top of the game page -> <appid>_hero.png
    logo.png     1280 wide  transparent logo drawn over hero   -> <appid>_logo.png
    icon.png     256x256    small icon

Needs Pillow and NumPy:
    python3 -m venv build/artenv && build/artenv/bin/pip install pillow numpy
    build/artenv/bin/python steam-frame/make_library_art.py
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets' / 'citravr_logo.png'
OUT = Path(__file__).resolve().parent / 'library-art'
EDITION = 'STEAM FRAME EDITION'
FONTS = ['/System/Library/Fonts/Avenir Next.ttc', '/System/Library/Fonts/Supplemental/Arial Black.ttf',
         '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf', r'C:\Windows\Fonts\arialbd.ttf']

# Columns of the source logo (3138x778) holding each part.
LIME_X = (0, 900)
CITRA_X = (930, 2360)

# Palette from the logo: lime greens over a deep blue-green night.
LIME = (96, 214, 143)
LIME_LIGHT = (160, 236, 186)
NIGHT_TOP = (8, 24, 30)
NIGHT_BOTTOM = (3, 10, 14)


def font(size, bold=True):
    for path in FONTS:
        if Path(path).exists():
            try:
                # Avenir Next: index 2 is Demi Bold, 0 Regular (macOS .ttc order).
                return ImageFont.truetype(path, size, index=2 if bold and path.endswith('.ttc') else 0)
            except OSError:
                continue
    return ImageFont.load_default()


def cut_logo():
    """The logo with a transparent background and "Citra" in white."""
    rgba = np.asarray(Image.open(SOURCE).convert('RGBA')).astype(np.float32)
    rgb = rgba[..., :3]
    h, w = rgb.shape[:2]

    # Background: near-white pixels, except inside the lime's silhouette, where the
    # white lines between its segments belong to the picture. (A flood fill from the
    # border can't tell: those lines reach the lime's edge, while the flourish of the
    # "V" encloses background that the border can't reach.)
    white = rgb.min(axis=2) > 235
    lime_fill = np.zeros_like(white)
    x0, x1 = LIME_X
    solid = ~white[:, x0:x1]
    rows = np.zeros_like(solid)
    cols = np.zeros_like(solid)
    for y in np.where(solid.any(axis=1))[0]:
        xs = np.where(solid[y])[0]
        rows[y, xs[0]:xs[-1] + 1] = True
    for x in np.where(solid.any(axis=0))[0]:
        ys = np.where(solid[:, x])[0]
        cols[ys[0]:ys[-1] + 1, x] = True
    lime_fill[:, x0:x1] = rows & cols
    outside = white & ~lime_fill

    # Soft-edged alpha for the coloured parts: pull the mask in by a pixel so no
    # white fringe remains, then feather it.
    mask = Image.fromarray(np.where(outside, 0, 255).astype(np.uint8))
    alpha = np.asarray(mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(1.2)),
                       dtype=np.float32)

    out = np.dstack([rgb, alpha])
    # "Citra": black antialiased on white, so its coverage is 1 - luminance. Redraw it
    # in white with that coverage as alpha.
    x0, x1 = CITRA_X
    text = rgb[:, x0:x1]
    coverage = 1.0 - text.mean(axis=2) / 255.0
    coverage = np.clip((coverage - 0.04) / 0.96, 0.0, 1.0)
    out[:, x0:x1, :3] = 255.0
    out[:, x0:x1, 3] = coverage * 255.0
    logo = Image.fromarray(out.round().astype(np.uint8), 'RGBA')
    return logo.crop(logo.getbbox())


def wordmark_of(logo):
    """ "CitraVR" without the lime."""
    part = logo.crop((CITRA_X[0] - 10, 0, logo.width, logo.height))
    return part.crop(part.getbbox())


def lime_of(logo):
    """The lime alone, cropped from the cut-out logo."""
    part = logo.crop((LIME_X[0], 0, min(LIME_X[1], logo.width), logo.height))
    return part.crop(part.getbbox())


def gradient(size, top, bottom):
    w, h = size
    t = np.linspace(0.0, 1.0, h)[:, None, None]
    colors = np.array(top, np.float32) * (1 - t) + np.array(bottom, np.float32) * t
    return Image.fromarray(np.broadcast_to(colors, (h, w, 3)).astype(np.uint8), 'RGB').convert('RGBA')


def glow(size, center, radius, color, strength):
    """A soft radial light."""
    w, h = size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xx - center[0]) ** 2 + (yy - center[1]) ** 2) / radius
    a = np.clip(1.0 - d, 0.0, 1.0) ** 2 * strength
    layer = np.zeros((h, w, 4), np.float32)
    layer[..., :3] = color
    layer[..., 3] = a * 255.0
    return Image.fromarray(layer.astype(np.uint8), 'RGBA')


def fit(image, width=None, height=None):
    if width is not None:
        return image.resize((width, round(image.height * width / image.width)), Image.LANCZOS)
    return image.resize((round(image.width * height / image.height), height), Image.LANCZOS)


def badge(text, size, padding):
    f = font(size)
    l, t, r, b = f.getbbox(text)
    tw, th = r - l, b - t
    w, h = tw + padding * 4, th + padding * 2
    layer = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle((0, 0, w - 1, h - 1), h // 2, fill=(*LIME, 235))
    d.text(((w - tw) / 2 - l, (h - th) / 2 - t), text, font=f, fill=NIGHT_BOTTOM)
    return layer


def background(size, lime_center, lime_radius):
    w, h = size
    bg = gradient(size, NIGHT_TOP, NIGHT_BOTTOM)
    bg.alpha_composite(glow(size, lime_center, lime_radius, LIME, 0.35))
    return bg


def paste_center(canvas, image, cx, cy):
    canvas.alpha_composite(image, (round(cx - image.width / 2), round(cy - image.height / 2)))


def lime_with_glow(lime, width):
    small = fit(lime, width)
    pad = width // 3
    layer = Image.new('RGBA', (small.width + pad * 2, small.height + pad * 2), (0, 0, 0, 0))
    halo = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    halo.paste((*LIME_LIGHT, 255), (pad, pad), small.split()[3])
    layer.alpha_composite(halo.filter(ImageFilter.GaussianBlur(width / 10)))
    tinted = np.asarray(layer).astype(np.float32)
    tinted[..., 3] *= 0.55
    layer = Image.fromarray(tinted.astype(np.uint8), 'RGBA')
    layer.alpha_composite(small, (pad, pad))
    return layer


def make_capsule(wordmark, lime):
    size = (600, 900)
    canvas = background(size, (300, 360), 560)
    paste_center(canvas, lime_with_glow(lime, 360), 300, 340)
    paste_center(canvas, fit(wordmark, 440), 300, 640)
    paste_center(canvas, badge(EDITION, 26, 12), 300, 760)
    return canvas


def make_wide(logo, lime):
    size = (920, 430)
    canvas = background(size, (460, 200), 560)
    paste_center(canvas, fit(logo, 760), 460, 190)
    paste_center(canvas, badge(EDITION, 24, 11), 460, 330)
    return canvas


def make_hero(lime):
    # No text: Steam draws logo.png over the hero.
    size = (3840, 1240)
    canvas = gradient(size, NIGHT_TOP, NIGHT_BOTTOM)
    canvas.alpha_composite(glow(size, (2900, 600), 1500, LIME, 0.32))
    big = lime_with_glow(lime, 900)
    shaded = np.asarray(big).astype(np.float32)
    shaded[..., 3] *= 0.9
    paste_center(canvas, Image.fromarray(shaded.astype(np.uint8), 'RGBA'), 2900, 600)
    return canvas


def make_logo(logo):
    canvas = fit(logo, 1280)
    with_badge = Image.new('RGBA', (canvas.width, canvas.height + 120), (0, 0, 0, 0))
    with_badge.alpha_composite(canvas)
    b = badge(EDITION, 44, 18)
    with_badge.alpha_composite(b, ((canvas.width - b.width) // 2, canvas.height + 30))
    return with_badge


def make_icon(lime):
    size = (256, 256)
    canvas = Image.new('RGBA', size, (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((0, 0, 255, 255), 48, fill=(*NIGHT_TOP, 255))
    paste_center(canvas, fit(lime, 200), 128, 128)
    return canvas


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    logo = cut_logo()
    lime = lime_of(logo)
    wordmark = wordmark_of(logo)
    outputs = {
        'capsule.png': make_capsule(wordmark, lime),
        'wide.png': make_wide(logo, lime),
        'hero.png': make_hero(lime),
        'logo.png': make_logo(logo),
        'icon.png': make_icon(lime),
    }
    for name, image in outputs.items():
        if name not in ('logo.png', 'icon.png'):
            image = image.convert('RGB')
        image.save(OUT / name, optimize=True)
        print(f'{OUT / name}: {image.width}x{image.height}')


if __name__ == '__main__':
    main()
