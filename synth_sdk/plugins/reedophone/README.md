# windwood

A clarinet by physical modelling: a reed, the air flowing past it, and a bore, simulated sample by sample. No
samples and no stored waveforms. Every constant was fitted to recordings of a real clarinet (the Crisis clarinet
set) by an automatic optimiser scoring how alike the two sound.

It departs from the real instrument in one place, for playability: there is no register vent. A real clarinet
overblows into its second register at written B4, where the tone changes abruptly; here the low register's tone
carries on smoothly over the whole keyboard (MIDI 36 to 99 speak, in tune, at every velocity).

## Knobs

| encoder | function |
|---|---|
| blue | breath: how hard you blow. With velocity and the stock envelope it sets how far above the reed's oscillation threshold the note sits. Soft notes are nearly sine-like and slow to speak; loud ones are bright and reedy. The output gain makes up 3/4 of the level change (the bottom is ~7 dB below the default instead of ~18, the top +0.7 dB instead of +2.8), so the knob is mostly timbre; `analysis/breath_sweep.wav` |
| green | growl: centre = none. Down: a hummed growl, the player humming into the mouthpiece (rough, snarling; at the bottom, multiphonic chaos). Up: flutter-tongue, the tongue rattling in the air stream at 22–32 Hz (at the top, broken and irregular) |
| white | bore: brightness of the tone-hole lattice (cutoff of the bell reflection, x0.5 .. x2) |
| red | air: breath noise in the air jet |

Defaults (blue 80 %, others centred, red 25 %) are the fitted values. A clarinet has no vibrato, so neither does
this. Playing legato (a new note while the last still sounds) keeps the air column, as a slur. The wave already in the
bore is time-scaled to the new length at that moment (no jump in the delay reads, so no click).

**Attack is the stock envelope.** The breath follows the envelope (through a 2 ms smoother), so the envelope's
attack is the note's crescendo. A fresh note always starts the same way: the reed is held shut by the "tongue" for
46 ms, then speaks within a few ms, for every note and velocity. The recordings are matched with an attack of about
135 ms (a linear ramp; set it by ear on the OP-1); short attacks give hard, punchy starts.

## Screen

Clarinet sections with a hair dryer blowing into the mouthpiece, a saw, a vent and a microphone at the bell
(`reedscr.c`; `reedscr.svg` is made by `gen_screen.py` from the hand-drawn `reedophone.svg`; `screen.gif` shows it
moving). Each part has its knob's colour:

| encoder | part |
|---|---|
| blue | the dryer's dial turns (9 o'clock to 3 o'clock) and the dryer blows more wind lines (1 to 7) |
| green | the saw slides left or right (spinning all the time); off centre it touches a section, which shakes harder the further out it goes |
| white | the microphone rises (up to 30 px) |
| orange | the vent slides down (up to 38 px) and more wind comes out of it (0 to 5 lines) |

While a key is held the dryer's trigger is pressed and the key's valve opens (MIDI note + 7, mod 24, picks one of the 24
keys). The wind comes on line by line in step with the envelope's attack (the share of lines lit follows the
envelope's rise toward its peak, all lit once it peaks; at most one new line a frame, so even an instant attack
sweeps): the dryer's lines, then the vent's, then the bell's. After the last key is released they go out in the
same order, one every 50 ms. The engine only tells the screen which voices are gated, their notes, their
attack progress and a render count (`rv_scr_*`).

## How it works

After Guillemain, Kergomard & Voinier, *Real-time synthesis of clarinet-like instruments using digital impedance
models*, JASA 118 (2005):

- **Reed**: a damped mass-spring, `x'' / wr^2 + qr x' / wr + x = p` (resonance ~2.9 kHz), driven by the mouthpiece
  pressure.
- **Flow**: Bernoulli flow through the reed channel, `u = zeta (1 - gamma + x)+ sgn(gamma - p) sqrt|gamma - p|`,
  solved in closed form each sample (one square root, from a table).
- **Bore**: a waveguide whose round trip is half a period, on every note. The open end reflects through a
  low-pass (the tone-hole lattice) whose phase delay is compensated in the loop length. (An earlier version had a
  register vent above written B4, with a 3x longer bore, as on the real instrument; it was removed so the tone
  does not jump there.)
- **Blowing pressure**: the oscillation threshold is computed per note from the bore's loss and the embouchure,
  then breath x envelope adds the excess on top. A fresh note holds the reed shut with the "tongue" for 46 ms while
  pressure builds, so even pianissimo notes speak promptly. Meanwhile the pressure is capped below the reed's
  closing point (a fast loud attack would otherwise pin the released reed shut: a choke), and the cap lifts over
  25 ms after the release. Letting go of the reed sends a pressure pulse out of the bell (a click), so the output
  fades in over 17 ms from the release. Opening the reed gradually instead was tried: it changes the attack, and
  (with the register vent of the time) upper soft notes started on the wrong mode.
- **Growl** modulates the mouth pressure that drives the reed, so the reed and bore answer it nonlinearly (rough
  sidebands, not tremolo). Hum: a buzz (sawtooth through an 800 Hz low-pass) a tritone below the note, folded into
  85–170 Hz, the range where the sidebands it makes sound rough rather than like a chord; depth up to 1.3x the
  breath, and its pitch wanders more near the end, which tips the reed into chaos. Flutter: the tongue cuts into
  the air stream ~45 % of each cycle, about the pressure's mean (so soft notes keep speaking); depth up to 1.25x,
  rate 22 to 32 Hz with growing irregularity. Both have a level make-up, so the knob does not change loudness
  much. The knob's distance from the centre is squared, so the first half of each side stays in the subtle,
  realistic range and the second half runs into the chaos. `analysis/growl_sweep.wav` plays the knob from bottom
  to top.
- **Output**: the wave leaving the bell, a radiation high-pass, a body EQ and a gentle low-pass.
- **Tuning**: the nonlinear loop drifts a little sharp or flat with dynamics; `reed_tune.h` holds the measured
  error per note x dynamics (`tools/calib.py`) and the loop length is corrected by it. Mean error ~1 cent up to
  MIDI 99; above that the loop is too short (the reed's own resonance is near) and notes go flat and thin.

## Cost

Sample loops in Blackfin assembly (`rv_asm.h`), bit-exact with the C reference in `reedophone.c`
(`tools/asm_check.c` on the simulator, including saturating extremes; the device build renders exactly the same
samples as the host build). On the Blackfin simulator: 175 cycles per sample per voice. Mixtur measures 167 there
against its ~110 instruction-count estimate, so expect ~115 real cycles: about 7.5 % of the CPU for 6 voices.
Growl adds 25 (hum) or 37 (flutter) simulator cycles per sample while it is on (about 9 % for 6 voices).
25 KB of state (two 1024-sample wave lines per voice).

## How it was fitted (tools/)

- `clar_common.py`, `ref_*.py`: the reference recordings. The SFZ maps them (forte 01–20, mf 21–47, p 48–70) and
  YIN confirms the pitches.
- `clar_eval.py`: the evaluator. It renders the same notes and velocities through the real fixed-point engine
  (`render_notes.c`, including the firmware's envelope multiply) and compares the harmonic levels in the sustain,
  the 1/3-octave spectrum, the attack envelope, the harmonics-to-noise ratio, level evenness, tuning and speaking
  latency. It writes plots and an A/B WAV (reference, then synth, per note).
  `python3 clar_eval.py plugin:reedophone --set full --att 135 --plots --ab` (`RV_CFLAGS='-DX=1'` builds a variant)
- `proto.c`: a float prototype of the model. `fit.py` runs CMA-ES on it (`spec_v*.json`, results in `fits/`).
  `gen_params.py` turns a fit into `reed_params.h`.
- `calib.py`: tuning table. `bench.c`, `asm_check.c`: Blackfin simulator cycle count and bit-exactness.
  `stress.c`: random notes, slurs, absurd pitches and knobs, garbage voice memory, under the sanitizers.

Score (lower is better, dB-like; the floor, comparing each reference note with its neighbour, is ~6 on harmonics):
full set 10.65 (harmonics 6.1 dB, band 5.3, attack 2.9, tuning 0.7 cents). With the register vent it was 9.88:
the recordings' upper notes are second-register tones, which this no longer imitates. The two alternative models made along
the way are plugins too: `licorice` (FM) and `chalumeau` (classic STK waveguide). See their READMEs.
`analysis/compare_all.wav` plays ten reference notes, each followed by reedophone, licorice and chalumeau;
`analysis/eval_reed_v4.png` plots the full-set comparison; the A/B WAVs (`--ab`, ~23 MB each) are not kept in the
repo: `clar_eval.py` writes them.

Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
