bloodmoon: BLOODMOON, a dark organ with a haunted bell (kennel synth plugin for the original OP-1), by Nola Drift
(published as SammiBabe666, https://github.com/SammiBabe666/Project_Bloodmoon, commit 3cac835).

Blue BLOOD (harmonic richness), green ECLIPSE (bell strike level and ring), white HAUNTING (vibrato depth and pitch
wobble), red MOONRISE (vibrato speed). Eight presets in presets/ (the author's files, unchanged). Pack:
python3 tools/kennel/pack.py synth_sdk/plugins/bloodmoon -o build/bloodmoon.knl. GPL-3.0-or-later with the OP-1
firmware permission. Kennel changes: PROVENANCE.md.

Blackfin: bloodmoon_dsp.S runs the sample loop, bit-exact with the C (exact_check.py --sym), reading only the sine
table's first quadrant. Screen: python3 gen_svg.py writes bloodmoon.svg and bloodmoon_geom.h from bloodmoon-art.svg (the
author's scene); moon.c moves the parts and draws the knob names and values with the OP-1's own font.

Rate note (2026-11-03): R246 renders 128 samples at 88.2 kHz, 689.06 blocks a second. The vibrato steps (0.15 .. 9 Hz),
the vibrato delay and fades, the bell decay (Q16 level, same rates per block) and the BLOOD smoothing were written for
44.1 kHz and ran twice too fast (red 0.3 .. 18 Hz). They are re-derived at 88.2 kHz; the assembly takes the increments as
arguments and is unchanged. Check: `harness/rate_bloodmoon_test.c`.
