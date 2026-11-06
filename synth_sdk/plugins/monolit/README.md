# monolit

A three-oscillator synth with a four-pole ladder filter, for bass and leads. Each knob is a macro: it moves several
settings along a tuned path, so any position should sound usable.

## Controls

| Knob | Page 1 | Page 2 (press the encoder) |
|---|---|---|
| Blue | Shape: one saw, then saw + sub square, two detuned saws, three saws, and a wide stack | Body: osc 3's interval: SUB (-1 octave), UNI, 5TH, OCT, SUB2 (-2 octaves) |
| Green | Cutoff: 80 Hz to about 7 kHz, 60 % key tracking, never below half an octave over the note | Vel: how much velocity opens the filter and changes the level. At 0 it only changes the level, a little |
| White | Contour: the filter envelope. The left half goes from short pluck to longer pluck. The right half adds sustain, then a slow attack, for swells | Age: oscillator drift (1 to 8 cents) and a small random cutoff offset on each note |
| Red | Heat: resonance and drive together, with level compensation and the bass kept as resonance rises | (none) |

The amp envelope, glide (portamento) and play modes are the OP-1's own. Page-2 values start at 0, and 0 is a good
default for each.

## Screen

A monolith in a night landscape. The green ridge over it is the filter: narrow and steep when the cutoff is low,
wide when it's open. Heat lifts the summit and turns more of its tip red. The blue rings on the ground are the
oscillators: two to seven with Shape, rippling outward while the contour is open. The white dashed arc is the contour:
a short, low arc for a pluck, a wide, high one for a swell, with a dot that climbs as the contour rises. On page 2 the
paged knobs show their names and values at the top right.

`gen_svg.py` writes `monolit.svg` (moon, hills, horizon and the monolith). `gen_tables.py` writes `monolit_tables.h` (the cutoff curve).

## Presets

`presets.py` writes twelve sounds to `presets/` (OP-1 patch files; copy the folder to the OP-1's `synth/` folder in
disk mode, e.g. as `synth/monolit/`): ladder bass, pluck bass, sub drop, ladder ping, acid line, fat lead, fifth lead,
bright stab, brass swell, worn tape lead, wide pad and night drone. The four basses sit an octave down. Rendered
without the OP-1's envelope and effects, they land between -18 and -21 dBFS RMS; sub drop is darker at about -24.

## Status

- Host renders (`harness/audio.py monolit`, sanitizers on) are clean at every knob extreme. Output stays within about
  ±1.5 dB of -19 dBFS RMS across each knob's sweep. All knobs at 0 sounds dark, at about -26 dBFS.
- bcore budget: 24,227 packets per voice call, about 82 % predicted for six voices at C7..C8, about 14 % per voice.
- CPU pass (2026-10-30, branch `opt-monolit`): the sample loop is hand-scheduled Blackfin code (`monolit_dsp.S`,
  one pass per oscillator, the sub square and the ladder, 16 x 16 MACs instead of 32-bit multiplies), the knob and
  pitch work is cached and runs out of line only when a knob or the pitch changes, and the voices plus the block's
  scratch live in the L1 pool (state lease). Same samples: `kern_check.py` (the kernels against the C loop, which
  host builds still use) and `exact_check.py` with the pool off and on. Worst six-voice block on bcore about
  85k packets instead of 141k.
- No fixed order yet: the loader gives it a number from 32 to 63. Claim one in `ORDERS.md` before it ships.
- Not done yet: listening on the emulator and the OP-1, and a native audio review of the presets.
