hollowsaint: Cathedral of the Hollow Saint, a haunted hurdy-gurdy (kennel synth plugin for the original OP-1).

Blue DRONE (push: FOLLOW / fixed key C..B), green STRINGS, white WHEEL BUZZ (push: stroke rate steady / 1/4 / 1/8 /
1/8T / 1/16 of the tempo), red CATHEDRAL. Presets in presets/ (python3 presets.py rebuilds them). The screen is generated
by tools/gen_screen.py in the project repository. Pack: python3 tools/kennel/pack.py synth_sdk/plugins/hollowsaint -o
build/hollowsaint.knl. GPL-3.0-or-later with the OP-1 firmware permission.

Kennel version: the sample loops run as Blackfin kernels (hs_*.S, bit-exact with the C), and the cflags in plugin.json
turn on the owner's CPU cuts (shared room and relic tape, body combs at the room rate, lighter relic, cheaper bow
noise, the string at half rate above C5). Without those flags hollow_saint.c sounds as upstream 0.7.0, apart from the
rate fix below. See kennel ARCHITECTURE.md.

Rate note (kennel, 2026-11-03): on R246 a voice call is 128 samples at 88.2 kHz, so the voice runs at 44.1 kHz and the
room and bells at 22.05 kHz, not the 22.05 / 11.025 kHz the plugin was written for. kennel re-derived its rate-bound
constants at the real rates: every filter in Hz, the slap, rattle, pop and DC decays, the bells' decay and partial
cut, the drone and relic fades, the bow lift, knob and pressure smoothing, the tape's wow, flutter, sag and rate
hold, the pitch drift, the free-running wheel (it struck twice the tempo), the fixed drone key (an octave high), the
trompette's 120 Hz floor, the high-string switch (now at C5) and the screen's keys. The body combs keep 4.0 / 5.1 ms.
The shared room's diffusers and comb moved out of the voices into the bus at twice the length (7.1 / 10.9 / 46.8 ms
as written; 7 KB less memory). The non-shared build (without plugin.json's flags) keeps its per-voice room and body
lines at their old lengths.
