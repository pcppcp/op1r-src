# reedie: a reeded woodwind you can unhinge

A physical-model reed instrument for the OP-1. A nonlinear reed drives a two-tap bore, so it starts
as a clarinet, turns into an oboe, and keeps going into pipes that were never built. Four knobs:
air, chatter, lip pressure and bore.

The factory patch is a breathy oboe. Turn the red knob left for a dark, stopped pipe, right for a
leaky, over-belled one. Push blue for more player air than instrument, green for a kazoo-like reed
rasp, white for a biting, choked embouchure. Pitch stays locked while you do all of that (except at
the very top corner, which is deliberately allowed to overblow).

## Knobs (page 1)

| knob | name | what it does |
|---|---|---|
| blue | **breath** | player air. Audible before the reed speaks and under the tone; also changes how long the reed takes to start (20 – 66 ms) |
| green | **buzz** | reed chatter / slap. Broadband rasp and aperture rattle, no pitch change |
| white | **bite** | lip pressure. Left: loose, dull, breathy, slightly flat. Centre: natural. Right: bright, sharp, thin, choked |
| red | **bore** | clarinet at 25 %, oboe at 70 %. Below 25 % a dark stuffed pipe; above 70 % bell tilt plus leak turbulence |

Defaults: breath 20 %, buzz 0, bite 50 %, bore 70 %.

## Where the instruments are

- **Clarinet**: red at 25 % (8192). Odd harmonics, hollow, can be pushed bright with white.
- **Oboe**: red at 70 % (22937). All harmonics, reedy, nasal.
- **Between**: a continuous morph, not a switch. The middle is an odd half-cylinder/half-cone thing
  that does not exist; it is allowed.
- **Past the oboe**: the bore gets leaky. Noise from the breath stream is mixed into the output, so
  the far end is airy and rough, not just brighter.
- **Under the clarinet**: the pipe is stuffed and dark; the loop loses its top harmonics faster.

## How it works

- A cubic reed fold `u = A (x - x³)` is the nonlinearity; its fixed rest position supplies the
  asymmetry that gives the oboe its even harmonics.
- The bore is a fractional delay with two interpolated taps at `D` and `2D` (`D = fs / 2f0`). The
  reed's fold is inverting, so the `D` tap behaves as a closed pipe (odd harmonics) and the `2D` tap
  as an open pipe (all harmonics); blending them moves the instrument from clarinet to oboe.
- A one-pole loss at 3 f0 (2 f0 under the clarinet) and a pitch-tracking DC blocker close the loop.
  Gain is constant, so low notes sustain as long as high ones.
- Breath is one per-voice noise stream, high-passed at 200 Hz and low-passed at ~4 kHz; half goes
  into the bore, half straight to the output.
- Buzz gates the same noise into the bore and wobbles the reed rest position, without touching the
  delay, so the note stays in tune.
- Past the oboe, band-limited breath noise is added to the output as bore leakage.
- Output gain is trimmed across the bore, and the top is rounded (not clipped) above 28000.

All integer DSP, no floats, no libc, no division or 64-bit maths per sample. The plugin file builds
with no imports at all.

## Measured on the host

| | |
|---|---|
| pitch, bore 0 – 100 % at MIDI 48 | −13 … +26 cents (worst at 50 % bore) |
| pitch, MIDI 36 – 88 at bore 70 % / 25 % | −7 … +15 / −12 … +3 cents |
| single-knob sweep | peak ~29900, rms ~12.8k – 17.2k: no dead setting |
| all four knobs at maximum | peak ~31300, still under full scale |
| attack (default) | 152 / 80 / 44 / 26 ms at MIDI 36 / 48 / 60 / 84 |
| note-off | fade over ~100 ms; no click (post-off step ≤ pre-off step) |
| default, MIDI 48 | H2 0.49, H3 0.06 (oboe) |
| clarinet, MIDI 48 | H2 0.10, H3 0.29 |
| breath at maximum | centroid 3.3 kHz, between-harmonic noise 0.06 |
| past the oboe | centroid ~3.0 kHz, noise 0.026 – 0.075, pitch unchanged |

Measured by rendering the engine on the PC and running FFTs on the output; see `REEDIE_SPEC.md`
for the method and the design decisions.

## Known limitations

- No register break: every key is a fundamental, so the clarinet never goes to its clarion register.
- No double tonguing; fast repeats are the sequencer's job.
- The bore changes colour, not length: sweeping red does not bend a held note.
- Bite at the top is thin and choked, not a normal embouchure.
- All four knobs at maximum can push a few high notes (e.g. MIDI 82, 88) into an octave-ish overblown
  mode. Every other extreme is lock-free across MIDI 36–88; this corner is the intended "unhinged" end.
- Mid-bore is up to ~26 cents sharp; the real instruments and the knob ends are within ~15 cents.
- CPU has not been measured on hardware yet; the audio path has no library calls.

## Files and building

```sh
cd synth_sdk
python3 harness/audio.py plugins/reedie        # -> harness/out/audio_reedie.wav
python3 make_plugin.py plugins/reedie          # -> build/plugins/reedie.op1p
```

`reedie.wav` is the standard harness phrase at the factory settings. `REEDIE_SPEC.md` is the full
design and decision log.
