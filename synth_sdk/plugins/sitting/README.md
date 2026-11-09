# in a room — "I am sitting in a room" (FX)

In 1969/70 Alvin Lucier recorded himself reading a short text, played the tape back into the room, and recorded
that playback. He then played the new recording back into the room and recorded it again, and kept going.
Each pass reinforced the room's resonant frequencies and wore away everything else. After about thirty
generations the words were gone and only the room's chord was left.

`in a room` does the same on a loop. Press orange and it records the input (generation 0). After that, every pass
of the loop plays the recording through a room model, normalises it, and writes it back in place. You hear
the loop. The counter shows how many times the recording has gone through the room. The loop can be free
(seconds) or locked to the OP-1 tempo (1/16 note up to 2 bars), so the melting loop stays on the beat.

The FX list shows it as **in a room**. The source folder, the C symbols (`FX_sitting`, `SCREEN_sitting`) and
`"order": 7` keep the old name, so sounds saved with the old FX still pick the same effect (see "Name" below).

## Knobs

| knob | page 1 | page 2 (press) |
|---|---|---|
| blue | **SIZE**: longest wall 0.8 - 8 m (log); 6.1 m at 88 % | **LOOP**: length of the next capture. First 40 %: FREE 0.5 - 7.6 s (2.6 s at the default; over 3.8 s it records at 11.025 kHz). Then 6 lengths synced to the tempo: 1/16, 1/8, 1/4 (1 beat), 1/2 (2 beats), 1 BAR, 2 BARS (4/4) |
| green | **SHAPE**: CUBE 1:1:1, GOLDEN 2.618:1.618:1, BOLT 1.59:1.26:1, LUCIER 6.1 × 4.3 × 2.6 m | **ABSORB**: high-frequency loss per generation (none to a ~1.3 kHz one-pole) |
| white | **ERODE**: how hard each pass is filtered (resonator Q 8–80, room share 12–100 %; the rest is direct sound plus early reflections) | – |
| orange | **MIX**: dry to wet (wet = the loop) | press = **LATCH**: each toggle starts a new capture; with a synced length it **arms** it (press again to cancel) |

The "Lucier" dimensions (20' × 14' × 8.5') are a plausible guess for his Middletown living room. They were not
measured. Before the first capture the wet signal is silence, and the dirty scratch buffer is never played.
The page-2 LOOP default is FREE 2.6 s.

## Tempo sync

- **Arming.** With a synced length selected, an orange press arms the capture. The old loop keeps playing (or
  silence before the first capture). The capture starts on the next grid point that fits the length:
  - 1/16 starts on any 1/16 note.
  - 1/8 starts on an 1/8 note.
  - 1/4 starts on a beat.
  - 1/2 starts on beat 1 or 3.
  - 1 BAR and 2 BARS start on beat 1.

  The screen shows ARMED (blinking) and a blinking dot next to MIX.
- **Exact length.** The capture stops exactly the chosen number of 1/16 grid points later, at the output sample
  of the grid point. From then on every generation starts on the grid point that follows. The loop length
  follows the clock to the sample (44.1 kHz) and never drifts. At 97 BPM, 1 beat is 27278.35 samples, so
  generations alternate between 27278 and 27279 samples, and every start is within 1 sample of the ideal time.
- **The clock.** It is the stock metronome's beat clock, read once per block through `op1_beat_clock()`
  (`op1_plugin.h`). This clock exists whether or not the click is on. It runs at the OP-1 tempo (BPM setting or
  MIDI clock), restarts on a beat when the tape starts, and moves with the tape speed while the metronome follows
  the tape. It is the same clock the count-in and the bar accent use.

  The op1r sequencers (euclid, dots, oracle, peal) take their tempo from the same OP-1 tempo (player ticks, 96 per
  beat). However, each one counts its steps from where it was started, through a private counter that only
  advances while it is the current mode's player. So there is no shared step phase to lock to. The metronome clock
  is the OP-1's only global beat phase.
- **Bars.** While the clock follows the tape, beat 1 comes from the tape position (beat number = round(tape
  position / beat), a bar line every 4 beats, the same rule as the accent click). Otherwise, the first synced
  capture defines beat 1, and later captures keep that bar grid.
- **Stopped tape.** If the clock follows the tape and the tape is stopped, the clock stands still. A copy of it
  then runs on at the tempo, so arming and looping keep working. When the tape plays again, the loop follows the
  tape's clock.
- **Tape speed.** At a different tape speed the clock runs at that speed, and the loop follows it (at half speed,
  a 1/4 loop lasts 44100 samples). A loop captured at one speed and played at a slower one restarts more often
  instead of leaving a gap (see "Tempo changes" below). Note that with the tape stopped the LOOP page previews the
  length at the nominal tempo; capture while the tape runs at the speed you will play at.
- **Long loops.** A loop that does not fit in 3.8 s at 22.05 kHz is recorded at **11.025 kHz** (7.6 s, more
  lo-fi; the screen shows LO-FI). At 120 BPM, 2 BARS (4 s) needs 11.025 kHz. 1 BAR fits at 22.05 kHz down to
  67 BPM and at 11.025 kHz down to 34 BPM; 2 BARS fits at 11.025 kHz down to 67 BPM. A length that does not fit even then is halved, and the LOOP
  page says what will be recorded: `SYNC 2 BARS AS 1 BAR` at 50 BPM, or `SYNC 1 BAR AS 1/2` at 120 BPM with the
  tape at 25 % (the clock runs at 30 BPM there).
- **Tempo or tape speed changes while a loop plays.** The loop keeps its recorded material and stays on the grid.
  - If the clock gets faster, the end of each pass is cut. A 256-sample de-click covers the step.
  - If the clock gets slower, the loop restarts on the largest power-of-two part of its length (1 BAR, 1/2, 1/4,
    ...) that the recording still fills, within a factor of 1.41. Each part starts on its own grid inside the bar
    (a 1/4 part on a beat, a 1/2 part on beat 1 or 3). For example, a 1 BAR loop captured at 120 BPM restarts every
    beat with the tape at 25 %, and goes back to whole bars on the bar lines when the tape returns to 100 %. Once
    the recording has run out, the restart comes on the next such grid point, so the pass that was playing when
    the clock slowed down does not leave a long gap either. Every restart is a generation, so at 25 % the room
    erodes the loop 4 times as fast per bar.
  - A slowdown smaller than that (up to 1.41 times slower) keeps the full length: the material runs out before
    the next start, fades out over about 1.5 ms, and there is silence until the grid point.
  - A pass shorter than 1/8 of the loop (realigning to the bar, a clock jump) does not count as a generation, so
    its peaks do not jerk the normalisation gain.
  - Re-capture (two presses) to fit the new tempo.

  A jump of the clock (tape relocation, MIDI START) can lengthen or shorten one generation by the jump.
- **Fades.** A free capture fades 10 ms at both ends, as before. A synced capture fades only 1.5 ms, which keeps
  the downbeat's attack.

## How it works

- **Loop.** Up to 84096 int16 samples in the FX scratch buffer, plus a 2048-sample early-reflection line and the
  2048-sample crossfade pre-roll ring (176384 of 176460 bytes). Free loops longer than 3.8 s and long synced loops
  run at 11.025 kHz.
  - 22.05 kHz: capture uses a [1 3 3 1]/8 decimator, and playback uses linear 2× upsampling.
  - 11.025 kHz: the same decimator twice, and linear 4× upsampling.

  The room coefficients (SVF frequencies, reflection taps, DC blocker, absorption) are computed for the loop's
  rate.
- **Room.** The direct sound plus 3 early reflections (round trips across L, W and H, gain 0.36 each) feed 6
  resonators.
  - The resonators are tuned to the room's axial modes f = n·c/2D (c = 343 m/s): the 6 lowest modes of the three
    dimensions at or above 110 Hz.
  - Modes within 0.8 % of each other count as one degenerate peak and get a stronger weight.
  - Each resonator is a Chamberlin SVF with 32-bit state and a Q15 coefficient 2·sin(πf/fs). The coefficient is
    a Taylor series, computed only when the knobs change.
  - At 11.025 kHz, modes above 1.7 kHz (only in tiny rooms) are clamped to 1.7 kHz.
- **Each pass** then goes through a DC blocker (27 Hz), the absorption low-pass and the normalisation gain:
  - The gain is G·(T/S)·(P/S) per generation. S is the peak this pass wrote (before the knee), P is the peak it
    read, and T is −6 dBFS.
  - Each ratio is limited to ×4 / ×¼.
  - A soft knee above −2.5 dBFS limits overshoot.
  - A silent capture is never boosted.
- **Multiplies.** They are 16×16 (Blackfin MAC, forced with a small inline asm). At 22.05 kHz both output samples
  of a loop sample are made together, as before. Odd grid points and 11.025 kHz use a one-sample-at-a-time path.

## Screen (`lucierroom.svg`, `gen_svg.py`, geometry in `sitting_geom.h`)

- **Counter and tags.** The counter reads "GEN 07" ("REC" while capturing, "GEN --" before the first capture).
  Next to it, the loop length is shown in blue ("1 BAR", "1/16", "2.6 SEC"), and below that ARMED (blinking) or
  LO-FI (11.025 kHz).
- **Info lines.** They show the shape (green), the dimensions (blue), and the loudest room mode (white). While a
  page-2 knob is edited, the third line shows `LOOP 2.6 SEC`, `LOOP 7.5 SEC LO-FI`, `SYNC 1 BAR [LO-FI | AS 1/2]` (what the next capture will record at the current clock)
  or `ABSORB nn%`. While
  idle it shows `PRESS ORANGE` or `STARTS ON BEAT`.
- **Floor plan.** An L × W rectangle in blue that grows with the size knob, with a side section for H. The nodal
  lines of the active axial modes light up with their resonator's energy.
- **Loop waveform.** The loop fills the lane (min/max of 128 buckets). The part already rewritten is white, the
  rest is grey, the head is red, and the recording is red.
  - Under the lane, a synced loop shows its 1/16 grid points (up to 1 beat) or its beats, with bar lines long.
    A free loop shows seconds.
- **Knobs.** The knob row sits above the bottom edge. Each knob has a short label in its colour (SIZE, SHAPE,
  ERODE, MIX; on page 2: LOOP, ABSORB) and a 5-px bar on a dim track of the same colour.
  - Page-2 values are dashed.
  - The page-2 LOOP bar has ticks for FREE and the 6 synced lengths.
- **Previews.** `sitting_preview.png` comes from `python -I plugins/sitting/preview.py`. It shows 11 states (idle,
  armed, recording, synced generations, 2 BARS at 11.025 kHz, free, 1/16, deep erosion, the LO-FI and AS labels, a
  7.5 s free loop), run through
  `harness/sitting_test.c` with a modelled beat clock. The stock `harness/render.py "in a room"` still works but
  only shows the idle screen, because it never presses the latch.

## Name

`"in a room"` (with spaces) is safe end to end:
- `make_firmware.py` checks `islower()` (spaces allowed) and length ≤ 15.
- The `fx_types` row is a plain SQLite text value.
- The FX name compare (`fx_name_eq` → `streq_n`) compares exact bytes with the string's length.
- The OP-1 stores the FX name in the sound's JSON, and no file paths are built from FX names.

Sounds store the FX **by name**, and an unknown name asserts in `fx_types.cpp:42` ("unknown fx name"), which
locks the OP-1 at boot. For that reason `dispatcher.c` keeps accepting the old name "sitting" (`fx_old_names`).
The effect itself is chosen by the selector word (knobs[4] = MAGIC + order 7), which the rename keeps.

## Verification (host, `cc -DOP1_HOST`, UBSan trapping)

`harness/sitting_test.c` includes the plugin and models the metronome clock: a phase that counts down 0x100000 per
64-sample block or the tape step, wraps by the beat length, and rescales on a tempo change as `set_tempo` does.
The input is speech-like, into a dirty scratch buffer.

- **Grid and length.** Tested with 1/4, 1/2, 1 BAR and 2 BARS × 120 / 97 / 133.3 / 71 / 160 BPM, 26
  generations each:
  - The capture starts on the model's grid point.
  - Every generation start is exactly the model's grid sample.
  - Lengths are floor/ceil of the musical length, for example 22050 at 120 BPM and 27278..27279 at 97 BPM.
  - The maximum distance of a generation start from the ideal time is < 1 sample, so there is no drift.
  - 1/16 and 1/8 at 124 BPM: 178 / 89 generations, every start within 0.9 samples.
- **Tempo change** in the middle of a 1 BAR loop:
  - 120 → 132 BPM: that generation is 86055 samples, then 80181..80182 (bar = 80181.8). The end is cut each pass.
  - 120 → 108 BPM: 90821, then 98000 (silence before each bar line). The loop stays locked.
- **Slower tape** (`test_slow_tape`): 1 BAR captured at 120 BPM with the tape stopped, then the tape at 25 % (step
  0x40000, 0x3FFFF, 0x40001). The old code played 1 beat and then left the recording used up for about 6 s per
  bar (4134 blocks). Now every generation is 88199..88201 samples (1 beat at 25 %), the recording is never used up
  for more than 1 block, and back at 100 % every generation is a whole bar starting on a tape bar line. A 1 BAR
  capture made at 25 % runs as 1/2 at 11.025 kHz without gaps.
- **Normal speed is unchanged:** rendered output of synced loops (1/16, 1/4, 1/2, 1 BAR, 2 BARS at 50 - 133 BPM)
  is byte-identical to the previous version.
- **Long free loops** (`test_free_long`): 2.6 s (default, 22.05 kHz), 6.5 s and 7.5 s (11.025 kHz) loop with
  exact lengths and no clipping after generation 0.
- **Gain ramp** (`test_ramp_blocks`): the previous generation's gain now survives the block boundaries of a free
  loop's 256-sample ramp (it was lost after the first block, so the seam dipped to 4 - 13 % of the level; now
  26 - 92 %). This is the only change to free loops of 2.6 s and less.
- **Slow tempos.** 2 BARS at 120 BPM runs at 11.025 kHz with generations of exactly 176400 samples. 2 BARS at
  50 BPM runs as 1 BAR at 11.025 kHz (211680).
- **Tape.** With a stopped tape, the clock copy runs on (15 generations). With the tape playing from 9.3 beats,
  a 1 BAR capture starts at tape beat 12 (a bar line). At half tape speed, a 1/4 loop is 44100 samples.
- **Free mode** (before the crossfade and gain ramp were added) was bit-identical to the version before sync. Checked against the old `sitting.c` with the same input
  (two lengths, default and extreme knobs, 40 s, two captures): all erosion output is equal and the loop buffer
  is identical. Only the lo-fi monitor during REC differs: it now lags by one loop sample.
- **Room convergence**, measured as energy within ±2 % of a mode:

  | loop | gen 1 | gen 5 | gen 10 | gen 20 | gen 24 |
  |---|---|---|---|---|---|
  | 1 BAR, Lucier room, 120 BPM | 4 % | 34 % | 80 % | 96 % | 97 % |
  | 2 BARS, golden room, 11K | 28 % | 93 % | 99.8 % | 100 % | 99.9 % |

  1/16 loops are 120 ms long, so their tones snap to multiples of about 8 Hz, which is often outside ±2 %. This
  is inherent to a short loop.
- **Level.** Generation peaks of a 1 BAR loop settle at −5.4…−6.9 dBFS. Generation 0 reaches −0.4 dBFS.
- **Clicks.** At 1 BAR and 2 BARS the steps at generation starts are below the signal's normal 99.9th-percentile
  slope. At 1/16 – 1/2 with bright small rooms they are up to about 2× that (no hard click).
- **No garbage.** Idle output is ≤ 1 LSB with a dirty scratch buffer. Arming and then cancelling stays idle.
- **Robustness.** 40 random runs (knobs, presses, page flags, tempo 30–290 BPM, clock jumps, tape follow/speed
  toggles) and extreme knobs: no UBSan trap, and the guard words after the scratch buffer are untouched.
- `harness/sitting_test.c wav <sel> <bpm> <gens>` renders a synced loop to a WAV (e.g. 1 BAR at 120 BPM, about
  12 generations); demos are on https://github.com/ultralazr/op1-musica-obscura.

## CPU and block cost (bfin-elf-gcc -O2, objdump)

- **Erosion path** per 22.05 kHz loop sample (2 output samples): about 480 instructions, against 467 before the
  sync changes (same compiler).

  | part | instructions |
  |---|---|
  | 6 SVF resonators | 6 × 45 |
  | the rest, wet/dry for 2 outputs included | about 210 |

  That is about 240 instructions per output sample, about 3.5 % of a 400 MHz Blackfin (previously about 3.4 %).
  At 11.025 kHz the loop costs roughly 60 %. The beat clock costs about 60–80 instructions per 64-sample block.
- **Average block** (64 samples, eroding at 22.05 kHz): about 15.6 k instructions.
- **Event blocks.** These estimates come from objdump instruction counts, with about 50 cycles per libgcc
  `__divsi3`. None of these events uses a 64-bit division.

  | event | added instructions | block cost |
  |---|---|---|
  | knob change (room recalculation + 6 SVF coefficients, 32-bit divisions only) | about 2–2.5 k | about 1.15× |
  | capture start (bucket clear, 2 divisions) | about 1 k | about 1.06× |
  | synced capture end (≤ 32-sample fade, `start_erode`) | about 0.6 k | about 1.04× |
  | generation wrap (2 divisions) | about 0.3 k | about 1.02× |
  | several events in one block | – | about 1.2× |

  The 2048-entry reflection-line clear is spread over the recording (4 entries per recorded sample). Only a
  capture shorter than 512 samples (a clock jump) clears the rest at once, which costs about 1.25×. `init` resets
  state only and never touches the scratch buffer.
- **libgcc calls.** There are none in a per-sample path. Divisions run per block (beat grid at another tape
  speed, tape beat number), per capture, per generation, or on knob changes.

## Known limitations / to check on hardware

- **Clock offset.** If the FX renders after the metronome in the audio callback, every loop is shifted by a
  constant 64 samples (1.45 ms). Check by ear against the click and against a sequencer started with the tape.
- **Clock mode.** Whether the metronome follows the tape (`follows_tape`) depends on the OP-1's tempo/sync
  mode. Check bar alignment with the tape playing and stopped.
- **Count-in.** After a count-in recording, the accent click counts bars from the count-in position. This FX
  counts them from tape position 0.
- **MIDI clock.** Under MIDI clock the phase runs at the derived tempo, as the metronome does.
- **Other checks.** The CPU figure still needs measuring with the resource monitor, including AUDIO peak and all
  three positions running the FX. The GUI draws about 270–410 lines per frame.
- **Loop length** is taken when a capture starts (or arms), so turning LOOP while a loop plays only affects the
  next capture.
- **Large rooms** cluster in a low hum (112–169 Hz for the real Lucier room). Smaller rooms, CUBE or GOLDEN give
  clearer chords on the speaker.
- **Master position** assumes `op1_fx_scratch(2)` is a full 176460-byte area, like the other positions.
