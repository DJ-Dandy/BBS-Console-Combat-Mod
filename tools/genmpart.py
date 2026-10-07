#!/usr/bin/env python3
"""genmpart.py -> src/mpart_gen.h

Builds the small block of KH2 HUD art (the "MP" plate, the bar gradients and the bar's round end) that the mod draws its MP bar
with, and the check pixels used to recognise BBS's own gauge texture at run time.

Inputs (your own game files, extracted with the scripts in tools/):
    KH2_FIELD_PNG   remastered texture 1 of KH2's field2d/us/zz0field.2dd (1024x512)
    BBS_GAUGE_PNG   remastered texture 2 of BBS's arc/pc/p00common.arc      (1024x512, the gauge_01 texture)
"""
import os, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
kh2 = np.array(Image.open(os.environ['KH2_FIELD_PNG']).convert('RGBA')).astype(np.float32)
bbs = np.array(Image.open(os.environ['BBS_GAUGE_PNG']).convert('RGBA'))
assert kh2.shape == (512, 1024, 4) and bbs.shape == (512, 1024, 4)

# The patch is laid out in texel units of BBS's 512x256 gauge sheet; one unit = one KH2 HD pixel = 2x2 pixels of
# the 1024x512 remastered sheet, so the KH2 art is enlarged 2x.
PW, PH = 96, 54                  # patch size in units
OX, OY = 104, 184                # where it goes in the sheet (units) - an unused area

def up2(a):
    """2x enlargement of an RGBA float block, filtered on premultiplied alpha"""
    al = a[:, :, 3:4] / 255.0
    pm = np.concatenate([a[:, :, :3] * al, a[:, :, 3:4]], axis=2)
    chans = [np.array(Image.fromarray(pm[:, :, c]).resize((a.shape[1] * 2, a.shape[0] * 2), Image.BICUBIC)) for c in range(4)]
    out = np.stack(chans, axis=2)
    al2 = np.clip(out[:, :, 3:4], 0, 255)
    rgb = np.where(al2 > 0.5, out[:, :, :3] / np.maximum(al2 / 255.0, 1e-6), 0)
    return np.concatenate([np.clip(rgb, 0, 255), al2], axis=2)

patch = np.zeros((PH * 2, PW * 2, 4), np.float32)
def put(block, ux, uy):
    h, w = block.shape[:2]
    patch[uy * 2:uy * 2 + h, ux * 2:ux * 2 + w] = block

# "MP" plate: KH2 pixels x 309..361, y 73..95 (52 x 22 with its soft rim)
PILL = (2, 2, 52, 22)
put(up2(kh2[73:95, 309:361]), PILL[0], PILL[1])

def strip(col, y0, y1, ux, uy, w=8, pad=2):
    """one texture column, repeated sideways, with its end rows repeated above and below so filtering stays clean"""
    c = kh2[y0:y1, col:col + 1]
    c = np.concatenate([np.repeat(c[:1], pad, 0), c, np.repeat(c[-1:], pad, 0)], 0)
    c[:, :, 3] = 255
    n = c.shape[0]
    ys = (np.arange(n * 2) + 0.5) / 2 - 0.5
    y_lo = np.clip(np.floor(ys).astype(int), 0, n - 1); y_hi = np.clip(y_lo + 1, 0, n - 1); t = np.clip(ys - np.floor(ys), 0, 1)[:, None, None]
    big = c[y_lo] * (1 - t) + c[y_hi] * t
    put(np.repeat(big, w * 2, 1), ux, uy - pad)

BLUE = (60, 4, 8, 16)            # blue gradient, KH2 column 432, rows 78..94
strip(432, 78, 94, BLUE[0], BLUE[1])
GREY = (72, 4, 8, 16)            # the same gradient in grey (tinted by the game), column 438
strip(438, 78, 94, GREY[0], GREY[1])
BLACK = (84, 4, 8, 16)           # plain black of the plate's border
blk = np.zeros((40, 16, 4), np.float32); blk[:, :, 3] = 255
put(blk, BLACK[0], BLACK[1] - 2)

# Round left end of the bar, as KH2's: a rounded rectangle's left end that runs on to the right.  Drawn here (KH2's
# own end pieces are for bars of other heights): the black frame, 20 high with corner radius 5, and the inside, 16
# high with radius 3, in the blue and in the grey gradient.  Each has 2 units of margin around it: clear above, below
# and to the left, the bar's own colour to the right.
def round_end(w, h, r, pad=2, sub=8):
    """coverage 0..1 of the shape, (h + 2 pad) * 2 rows by (w + 2 pad) * 2 columns"""
    W, H = (w + 2 * pad) * 2, (h + 2 * pad) * 2
    xs = (np.arange(W * sub) + 0.5) / sub / 2 - pad
    ys = (np.arange(H * sub) + 0.5) / sub / 2 - pad
    X, Y = np.meshgrid(xs, ys)
    cy = np.clip(Y, r, h - r)
    inside = (X >= 0) & (Y >= 0) & (Y <= h) & ((X >= r) | (np.hypot(X - r, Y - cy) <= r))
    return inside.reshape(H, sub, W, sub).mean(axis=(1, 3))
def gradient(col, y0, y1, pad=2):
    """one texture column over the rows y0..y1, end rows repeated `pad` times, 2x enlarged: (n * 2, 1, 4)"""
    c = kh2[y0:y1, col:col + 1]
    c = np.concatenate([np.repeat(c[:1], pad, 0), c, np.repeat(c[-1:], pad, 0)], 0)
    n = c.shape[0]
    ys = (np.arange(n * 2) + 0.5) / 2 - 0.5
    y_lo = np.clip(np.floor(ys).astype(int), 0, n - 1); y_hi = np.clip(y_lo + 1, 0, n - 1); t = np.clip(ys - np.floor(ys), 0, 1)[:, None, None]
    return c[y_lo] * (1 - t) + c[y_hi] * t
def cap(item, r, col=None):
    ux, uy, w, h = item
    cov = round_end(w, h, r)
    blk = np.zeros(cov.shape + (4,), np.float32)
    if col is not None: blk[:, :, :3] = gradient(col, 78, 94)[:, :, :3]
    blk[:, :, 3] = cov * 255.0
    put(blk, ux - 2, uy - 2)
FCAP = (4, 30, 8, 20)            # frame
cap(FCAP, 5.0)
BCAP = (20, 32, 6, 16)           # inside, blue
cap(BCAP, 3.0, 432)
GCAP = (34, 32, 6, 16)           # inside, grey
cap(GCAP, 3.0, 438)

# colour of fully transparent pixels = black, so filtered edges do not pick up a light fringe
patch[patch[:, :, 3] < 0.5, :3] = 0
p8 = np.clip(patch + 0.5, 0, 255).astype(np.uint8)

# the area must be free in BBS's sheet
tgt = bbs[OY * 2:OY * 2 + PH * 2, OX * 2:OX * 2 + PW * 2]
assert tgt[:, :, 3].max() == 0, 'target area of the gauge sheet is not empty'

# check pixels: spread over the art of the sheet
rng = np.random.RandomState(7)
b16 = bbs.astype(np.int16)
def pick(mask, count, tol):
    """`count` pixels of `mask` whose 5x5 surroundings have nearly one colour (robust against filtering differences)"""
    out = []
    ys, xs = np.nonzero(mask)
    for i in rng.permutation(len(ys)):
        y, x = int(ys[i]), int(xs[i])
        if not (2 <= y < 510 and 2 <= x < 1022): continue
        blk = b16[y - 2:y + 3, x - 2:x + 3].reshape(-1, 4)
        if np.abs(blk - blk[12]).max() > tol: continue
        if any(abs(x - cx) + abs(y - cy) < 24 for cx, cy, *_ in out): continue
        out.append((x, y) + tuple(int(v) for v in bbs[y, x]))
        if len(out) == count: break
    assert len(out) == count
    return out
opaque = bbs[:, :, 3] == 255
colourful = np.abs(b16[:, :, 0] - b16[:, :, 2]) > 60           # these tell RGBA from BGRA
checks = pick(opaque & colourful, 12, 4) + pick(opaque & ~colourful, 12, 2)

with open(os.path.join(ROOT, 'src', 'mpart_gen.h'), 'w') as f:
    f.write('/* generated by tools/genmpart.py - do not edit */\n')
    f.write('#define MPART_W %d\n#define MPART_H %d\n#define MPART_X %d\n#define MPART_Y %d\n' % (PW * 2, PH * 2, OX * 2, OY * 2))
    for name, (x, y, w, h) in (('PILL', PILL), ('BLUE', BLUE), ('GREY', GREY), ('BLACK', BLACK), ('FCAP', FCAP), ('BCAP', BCAP), ('GCAP', GCAP)):
        f.write('#define MPART_%s_U %d\n#define MPART_%s_V %d\n#define MPART_%s_W %d\n#define MPART_%s_H %d\n' % (name, OX + x, name, OY + y, name, w, name, h))
    f.write('#ifdef MPART_DATA\n')
    f.write('static const struct { unsigned short x, y; unsigned char r, g, b, a; } mpart_checks[%d] = {\n' % len(checks))
    for c in checks: f.write('    { %d, %d, %d, %d, %d, %d },\n' % c)
    f.write('};\n')
    # run-length coded rows of RGBA: (count, r, g, b, a)
    flat = p8.reshape(-1, 4)
    runs = []
    i = 0
    while i < len(flat):
        j = i
        while j < len(flat) and j - i < 255 and (flat[j] == flat[i]).all(): j += 1
        runs.append((j - i,) + tuple(int(v) for v in flat[i])); i = j
    f.write('static const unsigned char mpart_rle[%d] = {\n' % (len(runs) * 5))
    for k in range(0, len(runs), 6):
        f.write('    ' + ' '.join('%d,%d,%d,%d,%d,' % r for r in runs[k:k + 6]) + '\n')
    f.write('};\n')
    f.write('#endif\n')
print('patch %dx%d px at (%d,%d), %d runs, %d check pixels' % (PW * 2, PH * 2, OX * 2, OY * 2, len(runs), len(checks)))
if len(sys.argv) > 1:
    out = bbs.copy(); out[OY * 2:OY * 2 + PH * 2, OX * 2:OX * 2 + PW * 2] = p8
    Image.fromarray(out).save(sys.argv[1])
    Image.fromarray(p8).save(sys.argv[1].replace('.png', '_patch.png'))
