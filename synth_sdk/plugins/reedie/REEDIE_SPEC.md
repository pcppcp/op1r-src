# reedie — a physical-model reed engine

## Problem statement

A synth engine that reproduces reeded woodwinds, specifically oboe and clarinet, and that keeps
going past both of them into instruments that do not exist. It must be able to sound breathy, buzzy,
and faithful; the extremes are allowed to be unhinged. Four knobs, six voices, no screen, no page 2.

## Solution

A single-reed waveguide engine: a nonlinear reed drives a two-tap bore reflection loop. The reed is
a saturating flow curve; the bore is a fractional delay whose reflection sign decides whether the
pipe is closed-open (clarinet: odd harmonics) or open-open (oboe: all harmonics). A one-pole loss
filter and a DC blocker close the loop. Breath is per-voice noise injected at the reed and mixed
into the output; buzz is reed chatter that modulates the aperture and injects rasp without touching
the loop length, so the pitch stays locked. Bite moves the reed's operating point. Past the oboe the
bell leaks band-limited turbulence, so the far end of the bore knob is a rough, airy pipe rather
than a slightly brighter oboe.

This is not a wrapper around an existing plugin. `palme` supplied the fractional-delay loop
pattern and `arc` the nonlinear-oscillator stability patterns; nothing in the tree models a bore.

## Knobs

| knob | name | meaning | 0 | 50 % | 100 % |
|---|---|---|---|---|---|
| blue | **breath** | player air: audible before the reed speaks, then a reduced bed | bone dry, reed speaks instantly | half air | air dominates, reed slow to speak |
| green | **buzz** | reed chatter / slap | clean tone | audible rasp | heavy kazoo-like chatter, pitch still locked |
| white | **bite** | lip pressure on the reed | loose: dull, flat, breathy | natural embouchure | biting: bright, sharp, thin and choked |
| red | **bore** | instrument geometry | dark, stuffed pipe (unreal) | between cylinder and cone (odd zone) | over-belled, leaky, rough (unreal) |

Factory defaults (`plugin.json`): `[6553, 0, 16384, 22937]` — breath 20 %, buzz 0, bite neutral,
bore 70 % (oboe side). A breathy oboe is the first impression and demonstrates three knobs at once.

### Bore placement

The knob maps to a conicity `c` and to two "outside the instrument" regions:

| position | value | `c` | behaviour |
|---|---|---|---|
| 0 – 25 % | 0 – 8192 | 0 | pure cylinder; the pipe is progressively stuffed/darkened and the loss corner drops to 2 f0 |
| 25 – 70 % | 8192 – 22937 | 0 → 32767 | odd-harmonic cylinder morphs into all-harmonic cone |
| 70 – 100 % | 22937 – 32767 | 32767 | pure cone plus bell tilt and a rising leak/turbulence mix |

Clarinet sits at **25 %**, oboe at **70 %**. The acoustically meaningful axis (reflection sign)
saturates at both real instruments. Below and above them the knob keeps moving bell colour (an
output tilt) and, above the oboe, bore roughness — both continuous, so the sweep never jumps.

## Curves

All knobs are raw `0..32767` and are interpreted once per block:

- breath `b = (k >> 1) + (k² >> 16)` — a little quadratic, so the first third is fine-grained and
  the top still reaches full scale.
- buzz `z = (k >> 1) + (k² >> 16)` — same shape, so low settings are rasp, not mush.
- bite `t = k - 16384` — signed, neutral at the centre.
- bore `p = k`, piecewise mapped to `c`, `ex_lo` and `ex_hi` as above.

Only `c` and the derived coefficients are recomputed per block; nothing is smoothed per sample
except the delay `D`, which follows a new note by half-step per block (about 3 ms).

## Voice architecture

```
   (firmware envelope applied to the output afterwards)
                            |
   noise --> HP 200 Hz --> LP ~4 kHz --+--> air (straight to output, heard first)
                                        |
                                        +--> (+) --> [ reed fold ] --> u --> [ bore 2 taps ] --> r
                                                   ^                          |  D, 2D           |
                                                   |                          |                  |
                                                   +-- bite rest --+          +--> [ 1-pole loss ]+
                                                                   |                  |
                                                          chatter (buzz)         [ DC block ]
                                                                                        |
   noise --> chatter LPF --> rasp / aperture wobble --------------------------------+
```

Per sample, in this order (mirrors `rd_render`):

1. `r = ((32768 - c) * y[wp - D] - c * y[wp - 2D]) >> 15` — both taps interpolated (fraction from
   `D`), both resonant at `f0` because `D = fs / (2 f0)`. The `D` tap enters with `+` and the `2D`
   tap with `-`, but the reed fold is itself inverting, so the effective reflection is inverting for
   the cylinder (odd harmonics) and non-inverting for the cone (all harmonics).
2. While `age < D`, the reflection is faded in, so a cold ring cannot sing stale audio.
3. Loop loss: `lp += ((r - lp) * aloss) >> 15` (one pole at `3 f0`, or `2 f0` under the clarinet),
   then the DC blocker (`corner = f0 / 4`), then constant gain `32760`.
4. One xorshift32 sample feeds everything noisy: breath, chatter and the starting chiff.
5. Chatter: rectified noise low-passed to ~47 Hz gates broadband rasp; the same envelope wobbles
   the reed's rest position by up to ±1/8 full scale.
6. Reed: `x = rest - r + wobble`, clamp to ±32767, `u = A (x - x³)`, minus the steady flow `u0`.
   `A` rises by up to 25 % over the first 34 ms (attack blow) and fades over ~100 ms on note-off.
7. Breath and chiff are added; the sum is written to the ring at `wp` (saturated).
8. Output: bell tilt (`col = r + tilt * (r - op) / 32768`, `op` a 1.5 kHz one-pole), gain trim,
   plus breath, plus leak turbulence past the oboe, then a soft ceiling.

## Tuning

`D = fs / (2 f0)` in Q16 samples from `OP1_PITCH_INC[0]`: one 64-bit division per block
(`2^47 / inc`), which is allowed; no division or 64-bit math per sample.

The loop filters add phase that is not proportional to `D`. It is corrected before the clamp:

```c
comp  = 2200 + 300 c/32768
comp -= 3355                              /* loss filter phase at its 3 f0 corner, ~D/10 */
comp -= (768 - rq8) * 3.5                 /* lower corner under the clarinet */
D += D * comp / 32768 + 0.5 sample
```

The correction is constant with pitch; an earlier `1/rq8` form went ~67 cents flat at the top of
the bore and was replaced. Measured on the host (see Verification): within ±26 cents across the
whole bore range and ±15 cents over MIDI 36–88 at the working settings.

Range clamp: `D` in `4 .. 2000` samples, i.e. `f0` ≈ 11 Hz .. 5.5 kHz, so `2D` stays inside the
4096-sample ring.

## Amplitude and stability

- Reed fold is clamped and bounded; the ring write is saturated with `op1_sat16`.
- Loop gain is 32760/32768 per sample and constant, so low notes sustain as long as high ones. The
  reed supplies the energy; the low-pass and DC blocker supply the loss and the phase.
- The loss corner is 3 f0 (2 f0 under the clarinet): the same roll-off as a fraction of the
  spectrum at every pitch, and enough H3 damping that the loop cannot mode-jump at intermediate
  blends.
- Bore starts empty: reflection ramped in over one `D`, then 8 bore periods (capped 2048 samples) of
  decaying noise chiff seed the mode.
- Output gain is trimmed per bore (`44000 - 11232 c/32768`), because the clarinet is quieter.
- Above 28000 the output rounds with slope 1/8 instead of hard-clipping, so the loudest corners stay
  under full scale without squaring off. A final ±32767 guard only catches the wildest corner
  (measured worst ~31300), where the knee alone could theoretically overshoot int16.
- `note_off` fades the reed drive over 4410 samples (~100 ms) instead of breaking the loop. Measured
  post-off sample steps stay at or below the pre-off step, so there is no click.

## The four behaviours

### Breath (blue)

- **Onset**: noise starts at sample 0 with a puff that decays to a bed over ~160 ms, while the reed
  pressure ramps in over `20 + 46 * breath` ms. Air is audible *before* the tone and no note is
  delayed in time. At breath 0 the puff still exists but the reed is immediate.
- **Sustain**: a bed at roughly a third of the onset peak.
- **Shaping**: one-pole high-pass ~200 Hz (kills rumble), then a fixed one-pole low-pass ~4 kHz.
  The fixed corner keeps the air *above* the note at all pitches instead of following the
  fundamental; it is the "breath over the instrument" sound, and it keeps high notes from turning
  into hiss.
- Half the air is injected into the bore, half goes straight out, so breath changes both the
  excitation and the perceived air. Velocity scales the level.

### Buzz (green)

Reed chatter: the aperture is modulated by a slow noisy envelope (~47 Hz), and the same envelope
gates a broadband rasp into the bore. Depth is `z`; the loop length is never touched, so **pitch
stays locked** — pure timbre. At 100 % the tone is a rasping, kazoo-like buzz but still recognisably
the same note.

### Bite (white)

Bite shifts the reed rest position (`0.268 .. 0.331`) and the drive (`19000 .. 14000`):

- down: reed sits further from closing, more drive → louder, duller, a little flat, more breath.
- centre: natural.
- up: reed closes sooner and is driven less → brighter, sharper, thinner; at the very top the tone
  is choked and nasal rather than silent, so the extreme is still playable.

An earlier, wider bite range could shut the reed completely (rms fell to ~1000, i.e. near silence);
the range was halved to keep every setting audible.

### Bore (red)

Two taps, weights `1 - c` and `c`. The `D` tap enters positively and the `2D` tap negatively, but the
reed fold is inverting, so the cylinder behaves like a closed-open pipe (odd harmonics) and the cone
like an open-open pipe (all harmonics). Because the geometry defines the effective
length, the blend is compensated to keep the fundamental in tune: sweeping red changes the
instrument's colour, not its pitch. Below 25 % the pipe gets darker and more damped (stuffed,
stopped); above 70 % the bell tilt brightens and band-limited turbulence from the breath stream is
mixed into the output, giving an airy, rough, "leaky" pipe. Both ends are deliberately outside the
real instruments.

Velocity (MIDI or sequencer) scales breath level and brightness and shortens `D` by up to ~6 cents.
Level scaling stays with the firmware.

## Voice state (36-byte slot: vtable + 32 bytes)

```c
typedef struct {
    uint32_t vtable;      /* +0x00 firmware-owned: never write */
    uint32_t rng;         /* +0x04 xorshift32, per voice */
    int32_t  D_q16;       /* +0x08 bore delay, Q16 samples (smoothed) */
    int32_t  lp;          /* +0x0c loop low-pass state */
    int32_t  dc;          /* +0x10 loop DC blocker state */
    int16_t  b1, b2;      /* +0x14 breath high-pass / low-pass */
    int16_t  chat;        /* +0x18 chatter modulator */
    int16_t  op;          /* +0x1a output colour low-pass state */
    uint16_t rel;         /* +0x1c samples since note_off */
    uint16_t wp;          /* +0x1e ring write index */
    uint16_t age;         /* +0x20 samples since note_on */
    uint8_t  gate;        /* +0x22 note held */
    uint8_t  pad;         /* +0x23 reserved */
} reedie_voice_t;
typedef char reedie_voice_fits[sizeof(reedie_voice_t) == OP1_VOICE_SIZE ? 1 : -1];
```

Static memory: `static int16_t rd_ring[6][4096]` = 48 KB, indexed by `op1_voice_index(self)`.
Synths get no scratch, so the rings must be static.

## Per-block vs per-sample

Per block: clamp/shape the knobs, compute `c`, `ex_lo`, `ex_hi`, `rq8`, `kcol`, `gain`; compute
`D` from the pitch increment with the phase correction; compute the DC, loss, chatter, ramp and
chiff coefficients.

Per sample: two interpolated reads, three MACs (loss, DC, tilt), one xorshift, one chatter MAC, one
fold (2 multiplies), two breath MACs, and the mix. No division, no modulo, no 64-bit math, no float,
no libc.

The device build has **no imports** (checked by `make_plugin.py`), i.e. no libgcc calls in the audio
path. CPU has not been measured on hardware; the per-sample work is comparable to `palme`.

## Files

```
synth_sdk/plugins/reedie/
  plugin.json       name "reedie", kind "synth", symbol ENGINE_reedie, 4 defaults
  reedie.c          engine: note_on, render, note_off
  reedie.wav        demo phrase rendered with harness/audio.py
  README.md         knob table, how it sounds, measurements, limitations
  REEDIE_SPEC.md    this file
```

No screen, no `order`, no `uses_sample`, no `stereo`.

Build and render:

```sh
cd synth_sdk
python3 harness/audio.py plugins/reedie
python3 harness/audio.py plugins/reedie --knobs 0,0,16384,22937
python3 make_plugin.py plugins/reedie
```

## Verification (light)

Host tests, one build each:

| check | result |
|---|---|
| pitch, bore 0 – 100 %, MIDI 48 | −13 .. +26 cents (worst case at 50 % bore) |
| pitch, MIDI 36 – 88, bore 70 % / 25 % | oboe −7 .. +15, clarinet −12 .. +3 cents |
| knob-extreme sweep, peak | ~29900 max for one knob at a time; ~31300 for all four at maximum (no wrap, under full scale) |
| knob-extreme sweep, level | single-knob rms ~12.8k .. 17.2k: no dead setting; the quietest combined corner (breath 0, buzz+bite max, bore 0) is ~7100, quiet but not silent |
| attack time (default) | 152 / 80 / 44 / 26 ms at MIDI 36 / 48 / 60 / 84 |
| note-off click | post-off max sample step ≤ pre-off step at every tested note |
| default character, MIDI 48 | oboe H2 0.49, H3 0.06 |
| clarinet, MIDI 48 | H2 0.10, H3 0.29 |
| breath max | spectral centroid 3.3 kHz, between-harmonic noise 0.06 |
| bore max (past oboe) | centroid ~3.0 kHz, noise 0.026 – 0.075, pitch unchanged |
| `-O2 -Wall -Wextra` | clean; 36-byte struct assertion passes |
| device build | `reedie.op1p` 4281 B, 0 imports |

Deliberately lighter than the `palme` / `mixtur` treatment: no exhaustive calibration tables, no
fuzz campaign, no long-run statistics.

## Known limitations

- **No register break.** Every key is a fundamental; the clarinet never enters its clarion register,
  so the 12th-vs-octave character difference is absent. Deliberate (Q16/Q20) and the largest
  fidelity gap.
- **No double tonguing.** Dropped deliberately (Q14); articulation is the sequencer's job.
- **The middle of the bore knob is an odd instrument** (half-cylinder, half-cone). Accepted (Q15) as
  the "doesn't exist" zone.
- **Extreme bite is not a tuned setting.** It is thin and choked by design.
- **All four knobs at maximum** can push a few high notes into an octave-ish overblown mode (H2
  dominant, e.g. MIDI 82 and 88). Every other single-knob extreme and the factory default are
  lock-free across MIDI 36–88. The corner is left as the intentional "unhinged" end.
- **Pitch error reaches +26 cents** at mid-bore (around 50 %), where the two taps have their largest
  relative phase error. The real instruments and the knob ends are within ~15 cents.
- **Very low notes** (below ~11 Hz) are clamped; no playable MIDI note is affected.
- **CPU is unmeasured on hardware.** The build is import-free and the instruction count is in the
  same class as the other loop engines, but the resource monitor has not been read.

## Alternatives tried and rejected

| prototype | why it lost |
|---|---|
| modal synthesis | too dull on low notes; the requested reeds need a real bore |
| twin feedback loop (2 rings) | unstable, and needed twice the memory |
| 3-D bore tap at `3D` | strong subharmonic/octave locking on most notes |
| fixed 2.4 kHz formant filter past the oboe | changed the spectrum less than the leak noise, and cost extra state |
| hard-clipped reed past the oboe | raised the noise floor but lost level and caused mode jumps |
| output-only short reflection comb | made the bore sound hollow and boosted H2 unnaturally |
| loop-corner lift past the oboe | brightened but destabilised the loop at several notes |

## Decision log

| # | decision |
|---|---|
| Q1 | physical model (waveguide), not spectral or sample-based |
| Q2 | one engine; the fourth knob is the instrument, not a modifier |
| Q3 | buzz = reed chatter/slap, with saturation riding along |
| Q5 | breath = literal player air, before and under the tone |
| Q6 | faithful at the low end, unhinged at the top |
| Q7 | fresh engine, not a revival of the removed `reedy` |
| Q8 | ~5–7 % CPU ceiling for all six voices |
| Q9 | velocity drives breath (air, brightness, slight sharp) |
| Q10 | stock bars, no custom screen |
| Q11 | name `reedie` |
| Q12 | light verification, no exhaustive campaigns |
| Q13 | white = embouchure/bite, neutral at 50 % |
| Q14 | double tonguing dropped |
| Q15 | bore: clarinet 25 %, oboe 70 %, unreal beyond both |
| Q16 | fundamental only, no register break |
| Q17 | air first, reed speaks late; no timing delay |
| Q18 | buzz is pure timbre, pitch locked |
| Q19 | continuous bore axis, no mode switches |
| Q20 | fundamental only (confirmed despite the fidelity cost) |
| Q21 | snap pitch, short smoothing, no glide between notes |
| Q22 | bite neutral at 50 %; factory default oboe-ish |

Implementation decisions, for the next person who touches the DSP:

| # | decision |
|---|---|
| D1 | reed = fixed rest position + cubic fold; steady flow `u0` subtracted before the bore |
| D2 | two taps at `D` and `2D`, blended by `c`; `c` saturates at 70 % and the top 30 % is colour |
| D3 | one-pole loop loss at 3 f0 (2 f0 under the clarinet); constant loop gain 32760 |
| D4 | no loop dispersion; phase error corrected in `D` with a constant term |
| D5 | bite range halved so the top is choked, not silent |
| D6 | past-oboe leak = band-limited breath noise mixed into the output |
| D7 | output soft ceiling at 28000 (slope 1/8) instead of a limiter |
| D8 | note-off fades the reed over ~100 ms; the loop is not broken |

Copyright (C) 2026 nvmsocool. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and
NOTICE).
