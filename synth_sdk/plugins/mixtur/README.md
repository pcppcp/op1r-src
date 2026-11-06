# mixtur — subharmonic organ (Mixtur-Trautonium)

Oskar Sala's Mixtur-Trautonium (1952) played the *undertone* series: frequency dividers locked to the
oscillator produced f/2, f/3, f/4 … below the played pitch, and the mixture was coloured by resonant formant
circuits into the dark, vocal, slightly unearthly sound of Hitchcock's *The Birds* (1963). Where the overtone
series gives a major chord, the undertone series gives a minor one. In `mixtur` the key you play is the top note.
Four sawtooth dividers sound its undertones f/n, all derived from one master phase so they never drift, and two
parallel band-passes shape the result into vowels.

## Knobs

| knob | page 1 | page 2 (encoder press) |
|---|---|---|
| blue | divider A undertone n: OFF, 1 … 24 | formant 1: 180 Hz … 4.07 kHz (unset = 392 Hz) |
| green | divider B undertone n | formant 2: 500 Hz … 5.66 kHz (unset = 917 Hz) |
| white | divider C undertone n | resonance Q 0.7 … 22 (unset = 3.2) |
| red | divider D undertone n | (no page 2: white and red share word 7) |

Defaults: A 1, B 2, C 3, D 5. For C4 that gives C4, C3, F2 and Ab1, an F-minor undertone chord under the key.
Duplicate n values are muted, because identical saws would only add up coherently (+6 dB). The level is
normalised to the number of distinct undertones. Changing an n while a note sounds fades that divider out and back
in (~12 ms each way), so there are no clicks. Velocity scales the level.

**Sub-audio undertones fade out.** An undertone below the audible range is not a tone: under a low C, f/5 is 13 Hz,
which is 13 clicks a second, and each one rings the formants. That sounds like crackle. So every undertone's level
follows its own frequency: full above 40 Hz, fading to silence at 25 Hz. On the lowest keys you hear fewer
undertones; from the middle of the keyboard up nothing changes. (Found on hardware: the defaults crackled on the
lowest C.) Undertones fade in over ~12 ms at note on, while the key's own saw (n = 1) starts at once.

Screen: the *undertone ladder*. Twenty-four strings hang from a bridge, and string n has length 1/n, so the ends
trace the hyperbola of the undertone series. The four selected strings are coloured. While keys are held, a slowed
master clock ticks: each divider's bead steps one notch down its string per tick, and the string flashes when the
divider wraps. These are the real divider counters, slowed down to a speed you can follow. Below the hyperbola are
the two formant curves on a log-frequency axis (150 Hz … 6 kHz): grey on page 1, blue and green on page 2. The
bottom row shows n per knob, or F1, F2 and RES on page 2.

## How it works

* Master phase `p` (u32). Each divider counts master wraps in `c_k` (0 … n−1). Its phase is
  `c_k·⌊2³²/n⌋ + (p>>16)·⌊2¹⁶/n⌋`, using table constants, which never exceeds 2³² − 65536/n. The dividers are exact
  and stay locked forever, with no division per sample.
* Between two master wraps every saw is linear in `p`, so the whole mixture is a single ramp `A + (p>>16)·B`. That
  costs one multiply per sample; `A` and `B` are recomputed only at wraps and at gain steps. All saw resets fall on
  master wraps, so one 2-sample polyBLEP (with a lookahead for the sample before the step) band-limits all four
  dividers at once. It removes 15–28 dB of aliasing compared with naive saws.
* The two Chamberlin SVFs run in parallel with Q13 coefficients. The input is scaled by the damping, so the
  band-pass has unity peak gain. A makeup gain over Q keeps the loudness roughly constant, and a soft knee (linear up to 0.7 of full scale,
  rounded above; it replaced a cubic soft clip that distorted at every level) sits
  at the output. The worst-case L1 norms of the filter (Q 0.7 at 5.6 kHz: lp 1.54, bp 2.24, hp 5.06) bound every
  state for the largest possible input. Because of that bound, the sample loop needs no clamps and every multiply
  is a 16 × 16 MAC.
* The voice memory is shared with the other engines. A per-voice signature (static array) tells a voice we left,
  which keeps its phase and filters (continuous on retrigger), from one another engine overwrote, which starts
  clean.

## Verification (host harness, scripts in the build agent's scratch dir)

* **Tuning:** single dividers n = 1 … 24 on C2 … C6 (70 note/n pairs). The FFT peak (4 s window, parabolic
  interpolation) of h1, h2 and h3 matches f/n within **0.29 cents** worst case, which is the FFT resolution at
  20 Hz; typical error is 0.01 cents. For the default mixture on C4, every spectral peak is an exact integer
  multiple of f/30, which shows the phase lock.
* **Level:** peaks −3 … −12 dBFS per voice over notes, mixtures and resonance. The mean RMS stays flat at
  −19.4 … −21.1 dBFS across the whole resonance range, thanks to the makeup curve. `audio.py` default phrase:
  −4.1 dBFS. In steady state, a scan of 864 extreme cases (Q 22, coincident formants, all n = 24 and so on) never
  reaches full scale. Only abrupt jumps of the stored words (for example resonance 0 → max) can briefly drive the
  soft clip.
* **Clicks:** changing n (3→5, off→2, 2→off, all four at once) and retriggering a sounding voice show no
  high-frequency burst above the steady-state level (ratio ≤ 1.0 against the louder side).
* **Robustness:** 80 random trials with 6 voices, notes 0 … 127, knob and page words changed every 5–50 ms
  (including all-0, all-max, negative or garbage words, and garbage voice memory) run with UBSan trapping enabled:
  no trap and no runaway. The longest single-voice run at full scale was 19 samples: a soft-clipped transient
  after an abrupt jump of a stored word.
* All knobs at 0 = all dividers off = silence. This is by design, and the screen shows OFF.
* Output level is +3 dB over the first version (`LEVEL`), so the peaks just reach the knee at velocity 127.

## CPU estimate

From `bfin-elf-objdump` of `mx_render` (-O2, bf524):

* Pass 1 (phase, mixture, polyBLEP): 31 instructions per sample, as a hardware loop.
* Pass 2 (2 SVFs, makeup, soft clip): 71 instructions per sample, as a hardware loop, with no 32-bit multiplies.
* Per block: about 700 cycles of setup, including one `__udivsi3` for the polyBLEP reciprocal. That adds about
  5.5 cycles per sample.
* Per master wrap: about 135 cycles, counting the 32-bit multiplies at 5 cycles each. That adds 0.8 cycles per
  sample at C4 and 3.2 at C6.

Total: about **110 cycles per sample per voice**. With 6 voices at 44.1 kHz that is 29 Mcycles/s, **≈ 7.3 % of
400 MHz**. The worst cases are about 7.9 % at C8, and +0.3 % while a divider fades.

On the Blackfin both sample passes run as hand-scheduled kernels in `mixtur_dsp.S`. They compute the same samples,
bit for bit, as the C loops in `mixtur_dsp.h`, which the host builds use (as does a Blackfin build with
`-DMIXTUR_C_REF`). Pass 1 takes 8 instruction packets per sample between master wraps and pass 2 takes 42, and no
library division runs per block for a held pitch. On bcore (packets, a lower bound for cycles; the test guest adds
about 1.1k per call) one voice call takes 8.8k packets instead of 14.1k, and six voices with random knobs take 50k
instead of 82k per block.

## Known limitations

* The polyBLEP reciprocal comes from each block's first pitch increment. Fast pitch bends inside a block make the
  band-limiting slightly less exact, but not out of tune.
* The 2-point polyBLEP is mild. With wide-open formants (F1/F2 at maximum, Q 0.7), the alias energy stays about
  −25 dB below the harmonics at any pitch. At the default formants it is −46 … −51 dB for mixtures; a lone n = 1
  saw reaches −36 dB at G7.
* The fixed-point integrators leave a tiny DC offset (about −61 dBFS per voice).
* The screen's master clock is a fixed visual tempo (140 ms per tick), not the note's pitch.
* Snapping the resonance from minimum to maximum in one step (preset load, not encoder turns) gives a short
  overshoot, which the soft clip limits.
