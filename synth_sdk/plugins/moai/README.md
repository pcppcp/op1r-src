# moai

A Minimoog-style synth for the OP-1: three oscillators (range, wave, interval, volume each), noise, a 4-pole
transistor-ladder low-pass with resonance, drive, feedback and saturation, a contour (filter envelope), and the Model
D's modulation (osc 3 in LO or noise into the pitch and the filter). Every key is a full
voice (6 voices). The amplitude envelope and the level are the OP-1's own (the envelope tab), as for every engine.

## Views

The home view is the simple one, OP-1 style; pressing an encoder opens a detailed view. On every view the four knobs
turn the four parameters named above the dials, and the OP-1 LFO modulates those (so switching views switches what
the LFO moves). The needles, values and drawings show where the LFO takes them while a note plays, as on the stock
engines (on home they follow the cutoff, emphasis and contour; on a view, its four knobs). The LFO's offset counts
twice here (the stock LFO at full amount moves a knob by only part of its range): at amount 100 it sweeps the whole
range. The sound number on blue is not doubled (one sound per step already).

| press | view | blue | green | white | red |
|---|---|---|---|---|---|
| (start) | **home** | sound 000 .. 120: the 32 oscillator setups (below) and 89 presets, numbered together in a fixed shuffled order (000: the initial sound; it wraps around) | cutoff | emphasis | contour: one knob from none, through plucks, to slow swells |
| | **home, Shift held** | detune | drive | amount (contour depth) | saturation |
| blue | **advanced**: the whole synth as a panel, in the Minimoog's sections (oscillators, mixer, modifiers, analog; rows 1 / 2 / 3 are the oscillators) | the four highlighted controls (in the knob colours); **Shift + turn** moves a highlight along the panel | | | |
| green (again: next) | **oscillators**, the Minimoog's mixer as rows: 1 / 2 / 3 and M (modulation and noise); the selected one (Shift + green: backwards) in colour; on the right the scope: the latest note's real output (while nothing plays: the selected oscillator's shape at its volume) | range: off, LO, 32' .. 2' | wave | frequency: -8 .. +8 semitones, **Shift**: cents (in LO: the rate, shown in Hz) | volume |
| | (M row) | mod mix: osc 3 .. noise | mod osc: pitch of osc 1 and 2, up to +-1 octave | mod filter: the cutoff, up to +-3 octaves | noise level |
| white | **filter** | cutoff | emphasis | amount (contour depth, 0 .. 8 octaves) | feedback: the output back into the filter input, up to 3x (fatter, overdriven; the level is compensated) |
| white again | **contour** | attack | decay | sustain | release |
| red | **analog**: the chain, in signal order; above, the sound's oscillators (each at its volume) and the noise (grey) on one scope: detune slides osc 2 and 3 along osc 1, drift wobbles them, drive and saturation round them into the soft clip (dashed: its limits) | detune in cents (CT: osc 2 up, osc 3 down) | drift (random pitch wander) | drive: into the filter, 1 .. 4x (warm, compressed: the filter smooths what it adds) | saturation: after the filter, 1 .. 8x and slightly off centre, then darker as it rises (grit and even harmonics the filter does not take away) |

Blue goes home from any view (and toggles home / advanced); red opens analog; white toggles filter / contour; green
cycles the oscillators.

**Home is a scene**: the moai of Ahu Tongariki at dusk (drawn in scene/moai_v3c.svg), the sound drawn as a place.
At rest it is only the scene. The sound number shows top left, large, in the OP-1's own font, when it changes (or home
comes back), blue when the blue knob turned it. A knob shows top right, with its name, while it turns; Shift shows all
four (the second layer: DET DRV AMT SAT). Both stay as they are for about a second after the last turn, then fade out
in one go (a third of a second; counted in frames, about 60 a second). While a knob turns (any of the four, either
layer) the strokes at the cutoff take its colour, drawn thicker, and fade back a third of a second after; what it
moves on the horizon lights in its colour too.

- **The drawing is filtered as the sound is**: each stroke is a frequency, the long outlines low and the short details
  high, and the filter's response there sets its brightness. A closed cutoff leaves the outlines, an open one shows
  the whole drawing; the emphasis lights the strokes at the cutoff white (the resonant peak; below the cutoff the
  response is spread twice as wide, so it lights many strokes). On a note it follows the note's cutoff, so with the
  contour the detail flashes in and fades.
- **The ridges** on the horizon (left), between it and the first moai: the filter's response as the filter view
  draws it (20 Hz at the left, 11 octaves across). In front, low: without the emphasis, at the cutoff the sound has now
  (on a note: the note's sweep). Behind it, higher: the response the knobs set, cutoff and emphasis: without emphasis
  it rolls off early, with it the plateau runs out to the cutoff and a bump rises there; it moves only when those knobs
  turn. Behind that, dim: the same where the contour takes the cutoff (its amount up), where it could go. Green while
  the cutoff turns (both), white while the emphasis turns (the back one), red while the contour turns (the dim one).
- **The island** on the horizon (right) is the contour as the home view drew it up to v3.19: its level over time (a
  quadratic axis, 0 .. 2.4 s, the key let go at 70 %: plucks get most of the width), as high as its amount (none
  without a contour; the contour knob raises the amount over its first 40 %); red while the contour turns.
- **Noise** is wind: streaks across the sky, behind the moai. **Detune** makes the lines shimmer, **drift** sways them.
- The oscillators, cutoff amount, drive and saturation are not drawn (the knobs and the other views show them).

**Turning, in encoder clicks** (the plugin measures a click: the smallest turn it sees, with and without Shift, once
it has seen it twice; not in the frames right after Shift is pressed or released, nor a turn cut short at the end of
the range; six identical single clicks in a row that it would count as several put a too small measure right):

| what | turn | Shift + turn |
|---|---|---|
| continuous values | as the stock knobs | fine: 1/256 of the range per click (on home, Shift is the second layer instead: detune, drive, amount, saturation at the plain speed) |
| cutoff (filter view) | as the stock knobs; on a note (within 5 cents, A4 = 440 Hz) it shows the note's name | finer: 1/2048 of the range per click (about 6 cents; a semitone in 17 clicks) |
| steps: range, wave | one step per 3 clicks | one step per 3 clicks |
| sound number (blue on home) | one per click, 120 -> 000 and back | (home + Shift: detune) |
| frequency | one semitone per 3 clicks | one cent per click |
| contour times (attack, decay, release) | as the stock knobs, with a light **detent on each note value** at the tempo: a click that ends within half a click of one lands on it, and it stays one more click | free, fine (1/256 of the range per click), no detents |
| advanced view | the highlighted control | moves the highlight one control per click |

**Contour times on the tempo**: note values 1/32 .. 4/1 with triplets at the OP-1 tempo (the metronome's BPM or MIDI
clock); a time on a note value follows tempo changes. Between them the time is free (milliseconds); Shift + turn
never lands on one. The contour view shows the note value with its milliseconds under it.

**The scope** (oscillator views) is the real output of the latest note, not a drawing. The engine copies that one
voice's output into a ring (one 128-sample block is 64 word copies, about half a cycle per sample of that voice);
everything else is the screen's job, at frame rate, on 65 points: a window of two cycles of the lowest oscillator
sounding (for long cycles the engine keeps every 2nd .. 16th sample), triggered on a rising zero crossing with
hysteresis, placed between samples, and the trigger that best matches the last frame wins; the trace is averaged with
the last one, so detune beating morphs instead of flickering.

**What is saved with the sound** (seven values: an engine has no more): the home knobs (setup, cutoff, emphasis,
contour) and detune, drive, drift. Everything else is for the session: the oscillators come from the setup and the
contour's attack / decay / sustain / release / amount from the contour knob, until a view edits them; turning the
setup or the contour knob sets them again, and after loading a sound or restarting they come back from its seven
values. Noise, feedback and saturation start from the setup / zero (the saturation is not saved: no word left).

**A setup is a whole sound**: turning to a setup (as to a preset) sets everything it sounds with: the home knobs,
detune, drive, saturation, feedback, and the drift back to its default; nothing is kept from the sound before it, so a
number always sounds the same. Only turning the setup knob does this: a loaded sound keeps its own saved words.

**Numbers, no names**: the blue knob on home picks one of 121 sounds, 000 .. 120: the initial sound (000), four
Chase sounds (001 .. 004), then the 32 setups and 84 patches of [the library](docs/modern_patch_library.csv)
shuffled together once in the code (gen_presets.py, fixed seed: a number is always the same sound; ten near-duplicate
patches are left out). The knob wraps around at both ends. The plugin shows only the number;
[docs/PRESETS.md](docs/PRESETS.md) is the key (name, category, setup or preset, the CSV row).

**Presets**: a preset is a whole sound: turning
to one sets its oscillators, cutoff, emphasis, contour, detune, drive, saturation and contour amount (and for a few,
osc 3 in LO as a vibrato or a motor). Tweak it from there and save it as any sound: the seven words hold the preset
number and its home and analog knobs, and the rest comes back from the preset on load (the contour amount scaled to
your contour knob). Turning the knob on to the next number replaces your edits, as with setups. From the library:
glide is not in this engine (ignored); osc 3 as an audio-rate modulator (FM) is not either, so those patches have osc 3
off; the "optional vibrato" leads (CSV 59, 69) and the subsonic motor (66) get osc 3 in LO with the modulation row
set. The siren (CSV 65) is replaced by a sound made here, at its number (036, since v3.22): French touch, filtered
disco chords (two 8' saws spread by the detune over a 16' saw, a low resonant cutoff opened by a short contour on each
stab, drive and saturation; play chords on the off-beats). A second pass (v3.14) made them heard: rendered one by one, the patches whose osc 1 was 32' moved
up an octave (they sounded at 16 .. 33 Hz) with their cutoff, and the darkest got their cutoff raised to about the
level of the Chase basses (gen_presets.py: LIFT; test/preset_levels.py checks every one). The patches are written for a full Minimoog: many drive hard, the average preset costs about 171
cycles per sample per voice (CLASSIC: 133), the costliest (115) 236.

**The initial sound** (the engine just chosen on a slot) is **000**: the bass of Jestofunk's "Can We Live", the one
that sweeps on each bar. Two 16' saws (osc 2 about 3 cents up) and a 32' square sub at full, cutoff 12 %, emphasis
80 %, contour 66 % (amount 70 %), drive 25 %. Matched against the record: its resonance falls from about 1 kHz to
270 Hz in 0.6 s on D#1, ours within about 0.13 octave of it along the way (rendered and measured with the test
tools). Play it low and short (the record's note: the sub on D#1, 39 Hz, i.e. MIDI D#3 with the 16' saws on D#2); the OP-1's own envelope (T2) shapes the
loudness, and Shift on T2 sets mono and portamento (glide), which the sound does not set itself. A fresh sound is the
only case where a preset's detune and drive are written without turning to it: plugin.json gives the four home knobs,
and words 5 .. 7 still all 0 (never set) mean the engine was just chosen.

**001 .. 004: Giorgio Moroder's "Chase"** (1978). 001 is the main bass (the left channel of the intro): an 8' saw over
a 16' triangle with a little 4' saw, cutoff 40 %, emphasis 60 %, a short contour (25 %, amount 30 %): a resonant blip
around 500 Hz that closes within 0.1 s, fitted against the record's notes harmonic by harmonic (play it on 8ths, around
C3). 002 is the right-channel bass, rounder (8' triangle, 16' saw, less emphasis). 003 a lead (two 8' saws, a slow
vibrato from osc 3 in LO; the record's lead was a Roland SH-2000) and 004 strings (three saws, detuned, a slow swell,
for the Solina part). The song changes as it goes: these are starting points, not copies.

**Osc 3 rests in poly / unison**: when 4 or more voices sound at once, osc 3 is switched off for every voice (a 23 ms
fade; back on at 2 or fewer) so chords and unison fit the OP-1's DSP. The oscillator view says POLY or UNI (all the
sounding voices on one pitch).

## Setups

By group (the numbers on the knob are shuffled with the presets: docs/PRESETS.md). 8' plays the key, 16' an octave down, 4' an octave
up; "+7" = 7 semitones up; volumes 100 % unless noted. A setup is a whole sound, as a preset is: turning to it sets
its oscillators and noise, writes its cutoff, emphasis, contour, detune and drive into the sound's words, sets its
saturation and feedback, and puts the drift back to its default (since v3.19: before, a setup kept the filter and detune
of the sound before it, so the same number sounded different depending on where you came from). The views edit all of
it from there. Tips: ACID wants staccato, BRASS a slower attack (contour view), WIND a slowly turned cutoff.

| # | name | osc 1 | osc 2 | osc 3 | noise | cutoff / emphasis / contour (%) | detune, drive / sat / fbk (%) |
|---|---|---|---|---|---|---|---|
| 060 | CLASSIC | 8' saw | 8' saw | off | | 27 / 49 / 40 | 35, 0 / 0 / 0 |
| 108 | MOOG BS | 16' saw | 16' saw | 32' saw (75 %) | | 24 / 25 / 35 | 30, 25 / 0 / 0 |
| 068 | SUB BASS | 16' triangle | 16' square (50 %) | 32' triangle (75 %) | | 38 / 0 / 30 | 20, 0 / 0 / 0 |
| 107 | FUNK BS | 16' square | 16' saw | 32' square (75 %) | | 28 / 40 / 30 | 20, 20 / 0 / 0 |
| 098 | PULSE BS | 16' narrow pulse | 16' wide pulse | 32' square (75 %) | | 30 / 20 / 30 | 20, 15 / 0 / 0 |
| 005 | ACID | 16' saw | off | off | | 18 / 85 / 30 | 0, 45 / 25 / 0 |
| 023 | OCT BASS | 16' saw | 8' saw (50 %) | 32' square (75 %) | | 36 / 20 / 30 | 20, 15 / 0 / 0 |
| 056 | FIFTH BS | 16' saw | 16' saw +7 (75 %) | 32' saw (75 %) | | 25 / 25 / 35 | 25, 20 / 0 / 0 |
| 048 | SAW LEAD | 8' saw | 8' saw | 16' saw (50 %) | | 45 / 30 / 40 | 35, 30 / 0 / 0 |
| 076 | SQ LEAD | 8' square | 8' saw | 16' square (50 %) | | 45 / 20 / 40 | 30, 20 / 0 / 0 |
| 080 | 5TH LEAD | 8' saw | 8' saw +7 (75 %) | 16' saw (50 %) | | 50 / 20 / 45 | 30, 25 / 0 / 0 |
| 009 | SCREAMER | 8' saw | 4' saw (75 %) | 2' saw (25 %) | | 45 / 50 / 40 | 35, 70 / 45 / 25 |
| 038 | SOFT LD | 8' triangle | 8' saw (50 %) | off | | 35 / 10 / 45 | 30, 0 / 0 / 0 |
| 067 | WHISTLE | 8' triangle | 4' triangle (25 %) | off | 25 % | 50 / 30 / 50 | 20, 0 / 0 / 0 |
| 101 | FLUTE | 8' triangle | 8' square (25 %) | off | 50 % | 40 / 10 / 75 | 20, 0 / 0 / 0 |
| 025 | CLARINET | 8' square | 8' square (75 %) | 16' triangle (50 %) | | 35 / 10 / 70 | 15, 10 / 0 / 0 |
| 082 | OBOE | 8' narrow pulse | 8' narrow pulse (75 %) | 16' wide pulse (25 %) | | 45 / 15 / 70 | 15, 10 / 0 / 0 |
| 022 | REED | 8' wide pulse | 8' saw (75 %) | 16' narrow pulse (50 %) | | 45 / 15 / 70 | 20, 15 / 15 / 0 |
| 070 | BRASS | 8' saw | 8' saw | 8' wide pulse (75 %) | | 30 / 10 / 70 | 30, 35 / 15 / 0 |
| 011 | STRINGS | 8' saw | 8' saw | 4' saw (50 %) | | 55 / 0 / 90 | 60, 0 / 0 / 0 |
| 100 | SOFT PAD | 8' triangle | 8' saw (75 %) | 16' triangle (75 %) | | 40 / 5 / 90 | 45, 0 / 0 / 0 |
| 018 | SHARK | 8' shark tooth | 8' shark tooth | 16' reverse saw (75 %) | | 45 / 20 / 50 | 35, 20 / 0 / 20 |
| 085 | ORGAN | 16' triangle | 8' triangle (75 %) | 4' triangle (75 %) | | 65 / 0 / 0 | 10, 10 / 30 / 0 |
| 066 | CLAV | 8' narrow pulse | 4' narrow pulse (50 %) | off | | 55 / 30 / 20 | 10, 20 / 25 / 0 |
| 112 | SQ STACK | 16' square | 8' square | 4' square (75 %) | | 45 / 10 / 35 | 20, 15 / 0 / 0 |
| 031 | OCTAVES | 8' saw | 4' saw (75 %) | 16' saw (75 %) | | 45 / 15 / 45 | 30, 15 / 0 / 0 |
| 065 | FOURTHS | 8' saw | 8' saw +5 | 16' saw (50 %) | | 45 / 15 / 45 | 25, 15 / 0 / 0 |
| 090 | MINOR | 8' saw | 8' saw +3 | 8' saw +7 | | 45 / 10 / 50 | 25, 10 / 0 / 0 |
| 063 | MAJOR | 8' saw | 8' saw +4 | 8' saw +7 | | 45 / 10 / 50 | 25, 10 / 0 / 0 |
| 006 | BREATH | 8' saw | 8' saw | off | 50 % | 40 / 20 / 60 | 30, 0 / 0 / 0 |
| 052 | PERC | 8' triangle | off | off | 75 % | 40 / 40 / 15 | 0, 25 / 0 / 0 |
| 058 | WIND | off | off | off | 100 % | 45 / 85 / 95 | 0, 0 / 0 / 0 |

## How the knobs work without key events (mm_ui.h)

The plugin API gives an engine its knob words and, per encoder press, a page flag the core toggles. A press is that
flag: the plugin clears it and switches the view (Shift is read where the core reads it). On a view other than home
the four knob words are borrowed: each frame the plugin reads how far a word moved, applies that to the view's
parameter and sets the word back (the home values wait in memory and return with the home view; the borrowed words
stay at the home values, kept off the ends so a knob always turns both ways). This is how op1r's MIDI CC feature
writes engine knob words (the live word plus the patch setter). A sound loaded while on another view (its words
jump) or another slot brings the home view back.

## How the sound works

* **Oscillators**: saw, pulse (saw minus a shifted saw) and their mixes, band-limited with polyBLEP; triangle; noise.
  Free running and slowly drifting in pitch, as analog ones.
* **Ladder**: four trapezoidal one-pole stages with the feedback solved inside the sample (zero-delay feedback), so the
  resonance sits on the cutoff pitch and self-oscillation starts at a feedback gain of 4 at every cutoff. Soft-clipped
  input (the drive goes there) and output, AC-coupled output (14 Hz), as the Minimoog's.
* **The whistle and the bass**: the input clip also limits the resonance (the feedback is clipped with the input, as
  on the real ladder), so a hot input crushes the whistle. The input runs with headroom, and the resonance's bass
  loss is made up at the output, not by pushing the input: the bass stays (unlike a real Model D) and the whistle
  sits on top of it. DRIVE still pushes the input, and a driven input tames the resonance, as on the real one.
* **Transistor stages** (driven): in DRIVE's top half (above 50 %), every ladder stage saturates, as the transistor pairs of a
  real ladder (the tanh-per-stage models: Huovilainen, D'Angelo & Valimaki): a driven bass gets brighter and buzzier
  through a low cutoff instead of only louder and compressed. Below a 2 kHz cutoff (where it is heard; above, the
  cheap form it takes would add a false brightness). Clean or lightly driven, the input clip alone is within a few
  dB of it, so below 50 % it costs nothing.
* **Drive and saturation**: the same cubic soft clip, before the ladder (drive) and after it (saturation: the output
  stage driven harder and off centre, with its own DC blocker, then a one-pole low-pass from about 16 kHz down to
  5 kHz as SAT rises, as an overdriven amp's output: it takes away the fizzy, folded-back top that sounded digital). Both lower the output level by 1 / sqrt(gain), so they
  change the tone more than the volume (about +-2 dB on a full patch; a quiet, dark sound still gets louder).
* **LO and modulation** (the Model D's controllers): LO is 6 octaves under 8' (4.1 Hz at C4), following the key as
  on the Model D (higher notes, faster); its frequency knob spans 4x as far, 0.65 .. 26 Hz at C4 (shown in Hz, at the
  last note). Any oscillator can be in LO: in the mix it ticks and thumps, and with emphasis up each edge pings the
  filter. Osc 3 in LO is the modulation source, read as a control signal at its own shape (triangle, saws, square
  for trills, pulses); its volume at 0 leaves it silent but running. The M row (green to the 4th row) mixes it with
  noise (MIX: the Model D's modulation mix), and sends it to oscillators 1 and 2 (PIT, up to +-1 octave: vibrato,
  trills, sirens) and to the cutoff (FLT, up to +-3 octaves: wah, wobble, a chop with the square). The depths are
  where the Model D's mod wheel would be (squared: fine at the bottom). In osc 3's audio ranges it does not modulate
  (that would be FM, which this engine does not do); noise works with any range. The screen says so ("SET OSC 3 TO
  LO"). Session values, as the oscillators; a setup or a loaded sound starts without modulation. Costs about 9 cycles
  per sample while it modulates, nothing otherwise.
* **Speed**: [PERFORMANCE.md](https://github.com/fungiboletus/op-moai/blob/71c58d5/docs/PERFORMANCE.md). Passes per oscillator pair and for the filter, one-cycle
  16 x 16 Blackfin multiplies (inline assembly with exact C twins), no division in the audio path.

## Tests (host, no OP-1 needed)

From the repository root: `make test` (sanitizers, a 16-bit range check on every multiply operand, then the scripts
and `mm_ui`), `make bench` (Blackfin cycles in the simulator, host == Blackfin output), `make preview`, `make demo`.

| tool / script | checks |
|---|---|
| `mm_ui` | the views and knobs against an emulation of the firmware (turns, presses, a turn racing a press, a sound loaded on another view, a slot change, the LFO on a view and its needles, the home Shift layer a Shift turn landing late (`MM_RACE_HOOK`), and the audio thread rendering before the frame takes a Shift click back (it derived the next sound and reset AMT: the bug a user hit, 2026-11-08; `mm_home_hold`), setup characters, LO and the modulation, the presets: stepping, the words they write, saved and loaded again; the initial sound; the words a save puts into the sound, from another view too; session edits not following into another sound) |
| `levels.py` | levels of typical settings (a report: prints, does not judge) |
| `filter_response.py`, `ring_check.py` | the ladder: resonance position / height / corner, self-oscillation (reports) |
| `osc_check.py` | oscillator pitch, level and aliasing (polyBLEP vs naive) (a report) |
| `env_check.py` | contour times, the contour knob, keyboard tracking, velocity (white box) (a report) |
| `cpu_guard.py` | osc 3 off from 4 voices, on with fewer (fails `make test` otherwise) |
| `fuzz.py` | the top keys with osc 3 in LO and the modulation at full, bent both ways, then 300 random knob / session settings (modulation too), pitches up to the top key and bends: no overflow, no clipping, no DC (fails `make test` otherwise) |
| `preview.py` / `demo.py` | every view as an image (pycairo) / a demo WAV |

## Kennel version

Imported from op-moai 71c58d5 (kennel PROVENANCE.md). On the Blackfin the sixteen filter loops are `moai_fc.S`,
generated by `gen_fc.py` from the same operations as `filter_chunk_t` in `moai.c`, packed into the Blackfin's parallel
slots, and the saw, pulse, triangle and shark oscillator passes are `moai_osc.S` from `gen_osc.py`: bit-exact with the
C (`synth_sdk/harness/moai_fc_cmp.py` and `exact_check.py`), about 24 % less work for six voices. Run `python3 gen_fc.py`
/ `python3 gen_osc.py` after changing the filter or the oscillators in `moai.c`, then both checks. Build the C loops on
the Blackfin with `-DMM_FC_C` / `-DMM_OSC_C`. Pack: `python3 tools/kennel/pack.py synth_sdk/plugins/moai -o build/moai.knl`.

## Rate (2026-11-03, kennel)

On the OP-1 (R246) a voice call is 128 samples at 88.2 kHz, 689 calls a second. Moai was written and tuned with host
renders at 44.1 kHz and 344 calls a second, so on the unit the cutoff sat half an octave high (the filter table an
octave high, the key tracking an octave low and half of it tracked), the resonance whistled an octave over the knob,
the contour (and its beat-synced times) ran half as long, the drift walked twice as fast, the DC blockers sat at 27 Hz,
the saturation's low-pass twice as high, and the LO rate readout showed half the rate. kennel derives all of it for
88.2 kHz, toward what the host renders played:

- `gen_tables.py`: `FS = 88200` (the curve above 8 kHz kept at the design's frequencies), `C4_INC` in 88.2 kHz steps,
  `MM_DEC` per 32-sample chunk in Q18 (the contour, `>> 10` where it is applied; the fast attack is 6 chunks, 2.2 ms),
  the resonance fade at the top (`MM_GG_K0`, `MM_GG_K1`, a quadratic within 1 % of the design's) and `MM_SAT_LP` (the
  saturation's 15.7 .. 5 kHz low-pass, within 0.5 %);
- the cutoff smoothing 2.5 ms (`>> 3` plus `>> 7` a chunk), the osc 3 CPU fade 23 ms, the drift's walk on every other
  block (its 2.9 ms step), the noise modulation one value per two chunks (0.73 ms; its pitch part still changes per
  block, twice as often as on the host);
- the DC blockers `>> 10` (14 Hz; `gen_fc.py`, `moai_fc.S` regenerated), `INC_MAX` 16.5 kHz, `GG_TR` 2 kHz;
- the LO readout in Hz from 88.2 kHz increments.

Precision: at 88.2 kHz the lowest cutoffs have half the coefficient (20 Hz: gg 23, was 47), so the cutoff there is
quantised to about 2 % (the Q15 operand of the 16 x 16 kernels). The oscilloscope sizes its window from the key's
increment, so it was right already. `harness/rate_moai_test.c` (kennel, in `make check`) checks the cutoff at C4 / C5 and
the contour's decay in R246 units. `test/` still runs at 44.1 kHz.
