"""hdfont.py - the letters of the sub-menu headers (MAGIC, ITEMS, D-LINK), drawn in the style of the game's
"COMMANDS" label: slanted block letters with a heavy left stem, thin right strokes and a black drop shadow.

Nothing here is taken from the game's texture: every letter is a few rectangles and polygons on a 31 x 24 box,
sheared, softened and given a shadow.  The measurements (box, stroke widths, slant, shadow offset) were taken from
the game's own label so the new words sit next to it without standing out; tools/genhdart.py can draw "COMMANDS"
with this module next to the original to check that.
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

W, H = 31.0, 24.0          # letter box, in pixels of the 1024 x 1024 sheet
T = 5.3                    # thickness of the bars
SW = 12.5                  # heavy left stem
TW = 5.6                   # thin strokes
SLANT = 0.40               # pixels to the right per pixel up
GAP = 10.0                 # between letters
MID = 9.3                  # top of a middle bar
SHADOW = (3, 3)            # drop shadow offset
GREY = 130                 # the letters' grey (tinted by the game)

def R(x0, y0, x1, y1): return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
TOP, BOT, STEM, RSTEM = R(0, 0, W, T), R(0, H - T, W, H), R(0, 0, SW, H), R(W - TW, 0, W, H)
GLYPHS = {   # letter: (polygons, advance width)
    'C': ([TOP, STEM, BOT], W),
    'O': ([TOP, STEM, BOT, RSTEM], W),
    'M': ([TOP, STEM, RSTEM, R(16, 9, 22, H)], W),
    'A': ([TOP, STEM, RSTEM, R(16.5, 11.5, W, 17)], W),
    'N': ([TOP, STEM, RSTEM], W),
    'D': ([TOP, STEM, RSTEM, R(16, H - T, W, H)], W),
    'S': ([TOP, R(0, 0, SW, MID + T), R(0, MID, W, MID + T), R(W - TW, MID, W, H), BOT], W),
    'G': ([TOP, STEM, BOT, R(W - TW, 11.5, W, H), R(18, 11.5, W, 17)], W),
    'I': ([STEM], SW),
    'T': ([TOP, R(9, 0, 9 + SW, H)], W),
    'E': ([TOP, STEM, BOT, R(0, MID, 27, MID + T)], W),
    'L': ([STEM, BOT], W),
    'K': ([STEM, [(15.5, 9.5), (25, 0), (W + 1, 0), (20.5, 12.5)], [(15.5, 9.5), (22, 9.5), (W, H), (W - 7.5, H)]], W),
    '-': ([R(1, MID, 22, MID + T)], 23.0),
}

def word(text, pad=(6, 4, 8, 8), ss=4):
    """RGBA float image of a word: grey letters over a black shadow.  pad = left, top, right, bottom margins.
    Returns (image, x of the first letter's box, y of the box top)."""
    adv = [GLYPHS[c][1] for c in text]
    width = sum(adv) + GAP * (len(text) - 1) + H * SLANT
    w = int(np.ceil(pad[0] + width + pad[2])); h = int(np.ceil(pad[1] + H + pad[3]))
    im = Image.new('L', (w * ss, h * ss), 0); d = ImageDraw.Draw(im)
    x = pad[0]
    for c, a in zip(text, adv):
        for poly in GLYPHS[c][0]:
            pts = [((x + px + (H - py) * SLANT) * ss, (pad[1] + py) * ss) for px, py in poly]
            d.polygon(pts, fill=255)
        x += a + GAP
    # round the corners (blur, then harden), bring down to size, soften the edge as the game's art has it
    im = im.filter(ImageFilter.GaussianBlur(1.3 * ss))
    a = np.asarray(im, np.float32) / 255.0
    a = np.clip((a - 0.5) * 3.0 + 0.5, 0, 1)
    a = a.reshape(h, ss, w, ss).mean(axis=(1, 3))
    a = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32) / 255.0
    sh = np.zeros_like(a); sh[SHADOW[1]:, SHADOW[0]:] = a[:h - SHADOW[1], :w - SHADOW[0]]
    alpha = 1 - (1 - a) * (1 - sh)
    out = np.zeros((h, w, 4), np.float32)
    out[:, :, :3] = np.where(alpha > 1e-3, GREY * a / np.maximum(alpha, 1e-3), 0)[:, :, None]
    out[:, :, 3] = alpha * 255.0
    return out, pad[0], pad[1]
