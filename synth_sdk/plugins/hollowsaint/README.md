hollowsaint: Cathedral of the Hollow Saint, a haunted hurdy-gurdy (kennel synth plugin for the original OP-1).

Blue DRONE (push: FOLLOW / fixed key C..B), green STRINGS, white WHEEL BUZZ (push: stroke rate steady / 1/4 / 1/8 /
1/8T / 1/16 of the tempo), red CATHEDRAL. Presets in presets/ (python3 presets.py rebuilds them). The screen is generated
by tools/gen_screen.py in the project repository. Pack: python3 tools/kennel/pack.py synth_sdk/plugins/hollowsaint -o
build/hollowsaint.knl. GPL-3.0-or-later with the OP-1 firmware permission.

Kennel version: the sample loops run as Blackfin kernels (hs_*.S, bit-exact with the C), and the cflags in plugin.json
turn on the owner's CPU cuts (shared room and relic tape, body combs at 11 kHz, lighter relic, cheaper bow noise, the
string at 11 kHz above C5). Without those flags hollow_saint.c sounds as upstream 0.7.0. See kennel ARCHITECTURE.md.
