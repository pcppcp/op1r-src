#!/usr/bin/env python3
"""Regenerate the README screenshots with text (op1r/docs/images, text in Iosevka: harness/iosevka.py).
Each image is one panel of a host preview: its renderer is run into a temp PNG, the panels (black screens on the grey
sheet) are found, and the image's panel is cropped. Without a recorded panel index (None) the panel closest to the
current image is taken and reported (record it below).
    readme_previews.py [name ...]      (default: all)"""
import os, subprocess, sys, tempfile
from PIL import Image, ImageChops, ImageStat
HERE = os.path.dirname(os.path.abspath(__file__)); SDK = os.path.dirname(HERE)
IMG = os.path.join(SDK, '..', 'op1r', 'docs', 'images')
H = os.path.join(SDK, 'harness'); D = os.path.join(SDK, 'display')
# image: (renderer argv (OUT = the temp PNG), panel index or None, panel size)
JOBS = {
    'fx_ente':              ([os.path.join(SDK, 'plugins', 'ente', 'preview.py'), 'OUT'], 1, (960, 480)),
    'fx_steve':             ([os.path.join(H, 'render.py'), 'steve', 'OUT'], 4, (960, 480)),
    'fx_nostromo':          ([os.path.join(H, 'render.py'), 'nostromo', 'OUT'], 3, (960, 480)),
    'synth_aether':         ([os.path.join(H, 'render.py'), 'aether', 'OUT'], 0, (960, 480)),
    'synth_blot':           ([os.path.join(H, 'render.py'), 'blot', 'OUT'], 1, (960, 480)),
    'synth_dddd':           ([os.path.join(H, 'render.py'), 'dddd', 'OUT'], 0, (960, 480)),
    'synth_granular':       ([os.path.join(H, 'render.py'), 'granular', 'OUT'], 0, (960, 480)),
    'sampedit':             ([os.path.join(H, 'render_usbout.py'), 'OUT', 'sampedit'], 10, (960, 480)),
    'sysmon':               ([os.path.join(H, 'render_monitor.py'), 'OUT'], 1, (960, 480)),
    'seq_chord':            ([os.path.join(H, 'render_chord.py'), 'OUT'], 1, (960, 480)),
    'seq_grids':            ([os.path.join(H, 'render_grids.py'), 'OUT'], 2, (960, 480)),
    'tool_randomiser':      ([os.path.join(H, 'render_rnd.py'), 'OUT'], 0, (960, 480)),
    'sampler_plus_t1':      ([os.path.join(D, 'samplerplus_preview.py'), 'OUT'], 0, (640, 320)),
    'sampler_plus_t2':      ([os.path.join(D, 'samplerplus_preview.py'), 'OUT'], 2, (640, 320)),
    'sampler_plus_browser': ([os.path.join(D, 'samplerplus_preview.py'), 'OUT'], 4, (640, 320)),
}
BG = (64, 64, 69)                                     # the sheet colour (0.25, 0.25, 0.27)

def panels(im, size):
    """top-left corners of the size-sized screens: non-sheet pixel with sheet left and above"""
    px, (w, h), out = im.load(), im.size, []
    for y in range(1, h - size[1] + 1):
        for x in range(1, w - size[0] + 1):
            if px[x, y] != BG and px[x - 1, y] == BG and px[x, y - 1] == BG \
               and all(px[x + i, y] != BG for i in range(0, size[0], 8)) and all(px[x, y + j] != BG for j in range(0, size[1], 8)) \
               and (x + size[0] >= w or px[x + size[0], y] == BG) and (y + size[1] >= h or px[x, y + size[1]] == BG):
                out.append((x, y))
    return out

def run(name, cache):
    argv, idx, size = JOBS[name]
    key = tuple(argv)
    if key not in cache:
        tmp = tempfile.mktemp(suffix='.png')
        subprocess.run([sys.executable] + [tmp if a == 'OUT' else a for a in argv], check=True, capture_output=True)
        im = Image.open(tmp).convert('RGB'); cache[key] = (im, panels(im, size)); os.unlink(tmp)
    im, ps = cache[key]
    crops = [im.crop((x, y, x + size[0], y + size[1])) for x, y in ps]
    dst = os.path.join(IMG, name + '.png')
    if idx is None:                                   # closest to the current image
        cur = Image.open(dst).convert('RGB').resize(size)
        idx = min(range(len(crops)), key=lambda i: sum(ImageStat.Stat(ImageChops.difference(crops[i], cur)).mean))
        print(f'{name}: panel {idx} of {len(crops)} (closest; record it in JOBS)')
    crops[idx].save(dst); print('->', dst)

if __name__ == '__main__':
    cache = {}
    for n in sys.argv[1:] or JOBS: run(n, cache)
