# JUNI

A six-voice polysynth in the manner of the early-80s one-DCO polys, cut down to fit the OP-1. Each knob is a macro,
and every position of every knob should sound good.

## Knobs

| Knob | Main page | Page 2 (press; 0 is neutral) |
|---|---|---|
| blue | TONE: cutoff, with 60 % key tracking. The cutoff never closes more than 10 semitones below the note | RESONANCE, added to SWEEP's share. The top is just short of self-oscillation at every cutoff |
| green | SHAPE: saw + sub, saw, saw + pulse, PWM strings, saw + PWM + sub. All five are level-matched within about 2 dB | HPF: flat, then a one-pole up to about 800 Hz |
| white | SWEEP: how far the T2 envelope moves the cutoff (0 to 60 semitones, squared), plus up to 35 % resonance | PWM RATE: 0.9 to 7 Hz |
| red | CHORUS: off, then I (slow and deep), II (faster), I+II (fast and shallow) | (none: white and red share a word) |

T2 is the only envelope, as on the originals: it sets the amp after render, and the filter reads it through
`OP1_VOICE_ENV`.

## Design

- One DCO per voice. A half-rate phase gives the sub-octave square in bit 31. Saw and pulse read `phase << 1`,
  and the pulse is `saw(ph) - saw(ph + pw)`. polyBLEP corrects every edge.
- A 4-pole ladder (one-pole stages, last pole fed back one sample late), with a cubic soft clip on its input.
  `KSELF` (`gen_tables.py`) is the feedback at which it starts to self-oscillate for each cutoff. Resonance is a
  fraction of that, capped at 92 % and tapered to 55 % below MIDI 48, so low cutoffs don't boom. States run at half
  scale, so each pole costs one 16-bit fractional multiply. Rounding keeps the noise floor equal to the 32-bit
  version (checked).
- The chorus runs inside each voice, at 11.025 kHz: a 64-sample history (5.8 ms), two linearly interpolated taps
  with opposite triangle LFOs, then interpolation back up to 22.05 kHz. Each voice's LFO starts at its own phase,
  so a chord drifts like an ensemble. A one-pole high-pass (~250 Hz) on the wet keeps the bass dry, so the comb
  doesn't cancel low fundamentals, and the level holds within 0.8 dB from off to I+II (no dry normalisation). The rates (0.5 / 0.83 / 8 Hz) and depths are approximations tuned by ear,
  not measured from an original. The real stereo chorus would be a separate master FX.
- Synthesis runs at 22.05 kHz with 8-sample control steps, as Oxbow does, and four-point interpolation makes the
  128 native samples.

## Screen

The owner picked mockup A (2026-10-27): a midsummer night on a fjord. It is an image-model PNG with no vector file,
kept unchanged as `juni-art.png`, and `gen_svg.py` traces it with `display/art_trace.py` (docs/screen-art.md).

The fit fills the screen (2026-10-28, as TAFFY and WAVETABLE got). One uniform scale first: the 2:1 window
X -67..1603, Y 126..961 of the 1536 x 1024 mockup (black beyond its edges) is the screen, scale 0.192 (1.065 times the
first fit's 0.180). The height limits it: the top star sits at y 3.5 and the shore's bottom at 137.8, inside the OLED's
bottom margin. Then the two side groups move out sideways, whole, so the art spans x 2.2..317.8: the left shore with
the trees, the left mountain, the bonfire, the left cloud and star 18.4 px left; the right cliffs with the navy range
behind them, the right cloud and stars 18.1 px right. The middle cliff, the sun and its horizon stay. The green
waterlines run on to the fire and back to the horizon's end, and the reflection's outer dashes move out on a ramp, as
far as their slide and stretch allow at the right edge. Live sizes (the sun's travel, the ridges' waveform) scale with
the fit. `gen_svg.py` refuses art outside x 2..318, y 3..138, every moving part at its extremes included. Host renders
at the default and at each knob's ends: still ink x 2.2..317.8, y 3.5..137.8; live lines x 2.2..317.8, y 5.1..134.3
(was x 29..297, y 11..137); with the page-2 visuals, over every knob's ends: live x 2.2..317.8, y 4.1..134.3.
The still art is cached. The left mountain's ridge now follows its real outline over the peak (the first fit's piece
search took an inner line and drew a stray connector).
Colours are traced as measured and written as the nearest stock or encoder colour.

`juni.svg` holds the still parts, traced 1:1: the far ranges, the rocky shore with the fire's stones, the clouds,
the pines, grass, the cliffs' inner lines and the blue horizon. The stars are measured in the PNG and
drawn as small crosses and dots. It is drawn through the static-art cache (`op1_screen_draw_cached`, the whole
SVG is still). `juni.c` draws the moving parts from the same trace (`juni_geom.h`):

- blue, TONE: the sun (fitted disc, nine rays, the two broken halo arcs) sits where the mockup has it at the
  default TONE. At 0 it has sunk wholly into the fjord (16.7 px down) and is drawn dim; at the top it stands 8.2 px
  higher, as high as the halo allows under the top edge, and a third of the way to white. The halo grows from 0.75 to
  1.04 of the mockup's size over the knob and the rays grow a little.
- green, SHAPE: the three cliffs' ridge lines carry the DCO's waveform. Each ridge point is pushed out along its
  normal by the live wave (saw, pulse with the PWM, the sub an octave down), 0 to 5.9 px, a saw cycle every 12.8 px,
  faded out over 8.5 px at the ridge ends, so the outline keeps the mockup's shape and the waveform shows as its
  teeth: ramps with a step for saw + sub, a sawtooth for saw, crenellations that move with the PWM for the pulse
  shapes. The teeth breathe 0.8 to 1.2 of their depth with the PWM LFO.
- white, SWEEP: the bonfire (white tongues, red heart) is the mockup's size at the default SWEEP, 0.55 of it at 0 and
  1.3 at the top, and flares up to 18 % more with the voices' envelope. It flickers and sways as one flame, the
  sparks rise and start again, faster while notes sound.
- red, CHORUS: each traced reflection dash slides (up to 8 px with chorus I or II, 2 px of fast shimmer for I+II) and
  stretches (up to 45 %), rows in opposite phase, at the chorus LFO's rate; the voices' envelope adds up to 6 px at
  1.1 Hz, so the water moves whenever notes sound, chorus or not. Chorus off draws it darker.
- Page 2 (2026-10-28, after tester feedback that page 2 changed nothing): RESONANCE widens the halo's stroke up to
  1.6 times, whitens it up to 45 % and grows it 6 %; above three quarters it rings (the radius wobbles), the filter
  near self-oscillation. HPF thins the green waterlines to 0.55 of their width and dims them: the bottom loses its
  weight. The waterlines are drawn live for this (three short lines). PWM RATE is the rate of the screen's PWM LFO,
  the same law as the audio (0.9 to 7 Hz): the teeth's pulse width and breathing follow it.

Host check of every knob (OP1-Emulator `run/juni-fill-2026-10-28/sweep.py`, a held note, frame 40, 2x render):
pixels that differ between 0 and 99 are TONE 4977, SHAPE 1910, SWEEP 3926, CHORUS 5915, RESONANCE 4507, HPF 981,
PWM RATE 603 (was 0 for the three page-2 knobs). Live lines a frame 353 (357 before the fill), 21 % longer in total.

Preview: `juni_preview.png` (`harness/render.py juni`).

## Measured (2026-10-27)

Instruction packets per 6-voice block on bcore: JUNI 53.4k dry and 71.9k with the chorus, against Oxbow's 67.7k
in the same fixture. Oxbow reads 47 % AUDIO on the owner's unit, with red peaks. Packets don't count multi-cycle
32-bit multiplies, and JUNI uses fewer of them than Oxbow, so the real gap is probably smaller. Evidence:
OP1-Emulator `run/juni-load-2026-10-27/` (profile.py, profile.json).

Host checks (`juni_test.c`, sanitizers on): no clipped samples with six voices at all-max or all-zero; noise floor
between harmonics -132 dB dry; the four chorus modes measure as distinct (wobble depth 0 / 0.36 / 0.41 / 0.07 at
3.4 / 5.0 / 16 Hz). Renders go to OP1-Emulator `run/juni-host/`. These are source-only renders, without the stock
voice allocator, decimator or FX.

Not done yet: an emulator boot and listen, the CPU reading on the unit, OLED fit, and presets.
