#!/usr/bin/env python3
"""icon.png (128x128) and preview.png (at most 512x288) of the Mod Manager package, from thumbnail_src.png
(the picture supplied for the mod: the command menu in play).  Both show the whole picture, scaled to fit; the
icon is centred on a transparent square."""
import os, sys
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else HERE
src = Image.open(os.path.join(HERE, 'thumbnail_src.png')).convert('RGB')
w, h = src.size
k = min(512 / w, 288 / h, 1.0)
src.resize((round(w * k), round(h * k)), Image.LANCZOS).save(os.path.join(OUT, 'preview.png'))
k2 = min(128 / w, 128 / h)
small = src.resize((round(w * k2), round(h * k2)), Image.LANCZOS)
icon = Image.new('RGBA', (128, 128), (0, 0, 0, 0))
icon.paste(small, ((128 - small.width) // 2, (128 - small.height) // 2))
icon.save(os.path.join(OUT, 'icon.png'))
print(src.size, '-> preview', (round(w * k), round(h * k)), 'icon', small.size)
