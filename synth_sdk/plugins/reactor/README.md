# REACTOR

Status: Blackfin plugin with its screen and eleven presets, order 11 (packets 2..4, 2026-11-03; the runaway and the
living core, 2026-11-04, the owner's pick D of four proposals: OP1-Emulator `docs/reactor-sound-proposal-2026-11-04.md`). Design
and owner decisions: OP1-Emulator `docs/reactor-design-2026-11-03.md`. Internals and cost: kennel ARCHITECTURE.md
"REACTOR synth".

Four sine "rods" (ratios 1, 2, 3, 5) in a phase-modulation chain, rod 4 -> 3 -> 2 -> 1. Rod 1's output goes back
through a one-pole "coolant" lowpass into every pulled rod's phase. Each voice keeps a core temperature (Q16, 1.0 =
critical). HEAT, the rods, velocity and how long the note has been held raise it. COOLING lowers it, with a time
constant of 0.3 to 2 s. Past 1.0 the voice goes critical (it drops out below 0.8): loop gain 1.4x, rod index 1.27x,
and the overdrive gets 1.5x the drive, more asymmetry and a darker tone, gliding in over about 45 ms. Below critical the rod index already grows from 0.7x to 1x as the core
warms, so you can hear the build-up.

| Encoder | Name | What it does |
|---|---|---|
| blue | COOLING | coolant lowpass 12 kHz .. 300 Hz (log) and the cooling rate |
| green | RADIATION | noise into the coolant (up to 0.2), Geiger clicks up to 60/s, with more clicks when hot |
| white | RODS | rods 2, 3 and 4 come out one after another, a third of the travel each |
| red | HEAT | loop gain (up to 1.2 rad, 1.7 rad when critical), the overdrive (drive 1x .. 6x, tone), the core's heat source |

The overdrive (2026-11-03, the owner's pick "C" of three; the first version ran critical at 2.5x loop gain and 2x
index, and that chaos was mostly noise): a DC estimate (one pole, 6.9 Hz) is taken out first, since feedback FM leans
to one side. Then x = y x drive goes into the cubic soft clip (knee at |x| = 1), and a one-pole tone lowpass (about
17 kHz cold, darker with HEAT and once critical) keeps more drive from turning to fizz. The Geiger clicks go in before
the tone. Above about 1 kHz the modulation index scales down to keep aliasing in check. (The bias that clipped
off-centre for even harmonics, and the second DC estimate behind it, went on 2026-11-04 to pay for the runaway.)

The living core (2026-11-04), once per call: each rod's index drifts on its own random walk (4 % cold to 19 % near
critical; per voice, so chords shimmer), and the rods breathe at 0.3 Hz cold, quicker as the core heats (3.4 Hz at
the edge). Going critical sets a flare (the rods +0.9x, the loop +0.3x, the tone opens; 0.5 s). While critical the
movement moves after the loop: the drive throbs with the breathing and wanders with the flux, the tone breathes. The
critical loop sits at the edge of chaos, and moving its gain or the rods there turns to noise (Mac model: off-harmonic
energy -32 -> -17 dB with a 6 % wobble). Velocity squared is a strike at note on (120 ms): the drive up to 2x, the
tone open, the rods +0.37x (2x on the rods ran the loop into chaos at Nyquist). Heat darkens half as much as before
below critical and more once critical.

The runaway (2026-11-04), while critical: a fifth rod at half rod 1's pitch phase-modulates rod 1 (0.77 rad at
critical, up to 1.5 rad deep in meltdown): the loop's period doubles and an octave-down growl grows in. The pitch
sags under the load, -20 cents at critical and down to about -50 deep, wobbling with the flux, and comes back as the
core cools (`dinc`, added to every pitch step). In the kernel the rod's phase lives in `rx_run` (loaded and stored in
parallel slots), its product rides in R4.H to rod 1, and the call is split at the Geiger click events (`rx_render`),
so the loop has no branch.

## Files

- `reactor.c`: per-block control (temperature, rods, gains, click rate). It fills `rx_run` and calls the loop.
- `reactor_dsp.h`: `rx_run`, the kernel's constant ring, `rx_block_c` (the C reference: host path, `-DRX_C`) and
  `rx_render` (the call in pieces between click events). The DC estimate keeps its whole counts in the high half.
- `reactor_kernel.S`: the loop in Blackfin assembly, bit-exact with `rx_block_c`. About 83 packets a sample.
- `gen_tables.py` -> `reactor_sin.h`: the 2048-entry -sin table. The minus sign keeps the Mac model's sound: its
  sine helper (as fmbell's) returned -sin.
- `gen_svg.py` -> `reactor.svg`, `reactor_geom.h`: the screen, from the owner's image `reactor-art.webp` (the
  header says what is traced, what is measured and what moves). The screen code is at the end of `reactor.c`.
- `presets.py` -> `presets/*.aif`: eleven presets (2026-11-04: China Syndrome, Breeder, Ignition and Turbine new;
  Idle Hum, Cold Start and Cherenkov out). Word 7 trims come from `reactor_demo --level` (held-note levels -8.3 ..
  -6.1 dBFS before trimming, redone for the runaway). Source-only: the native preset review is open.
- The shake (2026-11-08, the owner's pick): as the core nears critical the plant buzzes, through critical and the
  runaway it judders (each part on its own: rods jump, the chimney sways, vessel, pipes and needles rattle); about 1 s
  after the last voice stops it is exactly at rest. Details: kennel ARCHITECTURE.md "REACTOR synth". Test:
  `harness/reactor_shake_test.c`.
- `reactor_demo.c`: Mac-only renderer. It plays 14 demos (sweeps and preset ideas) into WAVs and prints peak, RMS,
  clipping and critical timing (`--level k0 k1 k2 k3 [note]`: one held note's level). The build line is in its header. Output in the root project:
  `run/reactor-model/2026-11-03/` (`index.html`; the first-round renders are in `model-v1/`).

Cost (bcore, plugin_cost.py): voice 12,002, start 501. Six voices come to 72,513 packets, 34.7 % in the slot model
(synth slot 35 %; 33.6 % before the runaway, 30.3 % before the overdrive). The runaway first measured 13,020 (37.6 %);
the bias and its DC step, the click branch and a DC step's two shifts paid for it. Exactness: `harness/exact_check.py --save` with `#define RX_C 1` (the C loop), then `--compare` with the
kernel: 21 regimes the same.

## Rate (2026-11-03)

A voice call on R246 is 128 samples at 88.2 kHz (1.451 ms, 689 calls a second). The owner tuned REACTOR on the Mac model
(`reactor_demo.c`, which ran this engine at 44.1 kHz and 2.9 ms calls), so that model's sound is the design, and every
rate-bound constant was re-derived from it: per-sample poles as the model's pole taken in two steps (coolant table
12 kHz .. 300 Hz now 18830 .. 693, tone lowpass 17.3 .. 6.6 kHz via `TONE88`, DC poles 2^-11 and 2^-10 on states with
fraction bits, click decay 32273 = tau 0.75 ms), per-call steps halved or doubled (temperature 5/1024, boost glide
65/2048, rod smoothing 137/1024, `HELD_FULL` 2752 = 4 s with the creep slope halved, Geiger refresh every 16 calls,
`mean` in samples at 88.2 kHz), the aliasing taming still from 1027 Hz (`50000000`), and the two white-noise levels (loop
noise `nz`, click start) times sqrt 2, since white noise at twice the rate has half the density in the band. Pitch, the
phase table and the rods were right. What is not the model's: the feedback loop's one-sample delay is 11 instead of 23 us
(hot, critical settings differ in detail: -3.6 dB at 100-500 Hz in the hottest case against the 44.1 kHz render), and the
model's aliasing grit above 10 kHz in critical FM is not reproduced. `reactor_demo.c` now renders at 88.2 kHz (WAVs too).
Check: `harness/rate_reactor_test.c`. Cost: the kernel's packets a sample are unchanged (21 regimes bit-exact with the C
loop); the per-call control code is 19 packets longer (12,153 -> 12,172 at the defaults).
