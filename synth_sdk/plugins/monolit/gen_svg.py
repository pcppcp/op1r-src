#!/usr/bin/env python3
# Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
"""Writes monolit.svg: the static part of the screen, the night landscape: moon, planet, far hills, horizon and the
monolith. The moving parts (filter ridge, resonance tip, oscillator rings, contour orbit, page-2 labels) are drawn live
by monolit.c's screen code, which keeps them out of the monolith (MONO_* there must match MONO here).
Only what the OP-1's parser reads: polyline / circle, inline #rrggbb, plain numbers."""
import os

LAV, NAVY, DEEP, GREY = '#DED7FF', '#383572', '#1D2040', '#656579'
HORIZON = 104
MONO = dict(apex=(160, 32), shoulder_y=42, shoulder_dx=5, base_y=116, base_dx=10)


def pts(p):
    return ' '.join(f'{x},{y}' for x, y in p)


ax, ay = MONO['apex']
sy, sd, by, bd = MONO['shoulder_y'], MONO['shoulder_dx'], MONO['base_y'], MONO['base_dx']
left = [(ax, ay), (ax - sd, sy), (ax - bd, by), (ax, by), (ax, ay)]
right = [(ax, ay), (ax + sd, sy), (ax + bd, by), (ax, by), (ax, ay)]
hills_l = [(14, HORIZON), (14, 99), (26, 95), (38, 98), (52, 92), (66, 97), (84, 99), (104, 101), (130, 102), (146, HORIZON)]
hills_r = [(174, HORIZON), (190, 102), (214, 100), (232, 96), (246, 99), (262, 93), (278, 97), (294, 95), (306, 99), (306, HORIZON)]

parts = [
    f'<circle cx="40" cy="36" r="11" fill="{LAV}"/>',
    f'<circle cx="286" cy="64" r="4" fill="{NAVY}"/>',
    f'<polyline points="{pts(hills_l)}" fill="{DEEP}" stroke="{NAVY}" stroke-width="1.5" stroke-linejoin="round"/>',
    f'<polyline points="{pts(hills_r)}" fill="{DEEP}" stroke="{NAVY}" stroke-width="1.5" stroke-linejoin="round"/>',
    f'<polyline points="14,{HORIZON} 306,{HORIZON}" fill="none" stroke="{GREY}" stroke-width="1"/>',
    f'<polyline points="{pts(left)}" fill="{DEEP}" stroke="{LAV}" stroke-width="1.5" stroke-linejoin="round"/>',
    f'<polyline points="{pts(right)}" fill="{NAVY}" stroke="{LAV}" stroke-width="1.5" stroke-linejoin="round"/>',
    f'<polyline points="{ax - sd},{sy} {ax + sd},{sy}" fill="none" stroke="{LAV}" stroke-width="1"/>',
]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'monolit.svg')
with open(out, 'w') as fh:
    fh.write('<svg version="1.1" xmlns="http://www.w3.org/2000/svg" width="320px" height="160px" viewBox="0 0 320 160">\n')
    fh.write('\n'.join(parts) + '\n</svg>\n')
print(out)
