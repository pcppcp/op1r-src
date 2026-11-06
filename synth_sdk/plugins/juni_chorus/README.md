# juni chorus (FX)

A stereo ensemble chorus in the manner of the early-80s polysynth BBD chorus, with its three modes I, II and
I+II. It pairs with the JUNI engine, which only has a mono chorus per voice. Put this one at the master
position for the real stereo version. It also runs at the synth and drum positions (mono).

## Knobs

| knob | name | what it does |
|---|---|---|
| blue | MODE | I (slow, about 0-28 %), II (a little faster, 41-59 %), I+II (fast and shallow, 72-100 %). Between the bands the rate and depth glide linearly (12 % wide ramps), so turning it never clicks. |
| green | DEPTH | Size of the delay sweep, 20 % to 100 % of the mode's range. It never goes fully static. At full depth in I and II the delay sweeps 1.7 to 5.3 ms. |
| white | TONE | Darkness of the wet path: two one-poles in series, from about 8.6 kHz (left) down to about 1.3 kHz (right), as an old, tired BBD. |
| red | MIX | The wet rises 0 to 0.7 on top of the whole dry signal, as on the original (dry and wet together). 0 is the dry signal bit for bit. |

Defaults: MODE I (14 %, the middle of its band), DEPTH 85 %, TONE 18 %, MIX 75 % (2026-10-28, after tester feedback
that the old default, MODE II, DEPTH 73 %, MIX 64 %, was barely audible on a synth). MIX is linear: the wet's gain is
0.7 x MIX on top of the whole dry, so 75 % is a wet of 0.525 (about -5.6 dB against the dry before the wet path's
filters; measured -7 to -10 dB, see "Depth" below).

## Design

- One modulated delay per side, one triangle LFO, the two sides 180 degrees apart:
  - master (stereo): `L = dry + delay(L, centre + lfo)`, `R = dry + delay(R, centre - lfo)`.
  - synth/drum (mono): `dry + delay(x, centre + lfo)`: one tap and the dry, the stereo left side alone, as the
    original's mono output used one chorus side. Until 2026-10-28 the mono path summed both opposite taps; their
    pitch movements cancel (for a sine the sum is the centre tap times cos(w x sweep): level, no pitch), which left a
    faint comb sweep and, in I+II, almost nothing.
- Centre delay 3.5 ms (77 samples at 22.05 kHz). Linear-interpolated reads.
- Wet path: a one-pole high-pass at about 250 Hz first, then the two one-poles, then a very soft cubic saturation
  (`x - 0.12 x^3` at full scale). The high-pass keeps the bass dry: a 3.5 ms delay puts the first comb notch at
  about 143 Hz, where it cancelled the low notes' fundamentals (a C3 pad lost 6 dB in the first native take,
  2026-10-27). With it the level held within 0.3 dB at MIX 64 % and 100 % on three JUNI presets (host run over
  the native dry takes, with the old two-tap mono path; not re-run for the one-tap path and the new defaults). The high-pass state keeps 8 fraction bits, so it settles to zero after a sound.
- Rates (approximations): I 0.5 Hz, II 0.8 Hz, I+II 9 Hz with a sweep of +-0.45 ms. Published figures for the
  original disagree (for example 0.4 / 0.6 Hz against 0.5 / 0.83 Hz), and these were picked to sound right, not
  measured from hardware. The delay range, filter corners, saturation amount and the 0.7 wet level are approximations too.
- Not included: a hiss knob (there are only four knobs, the hiss would be inaudible at honest levels), BBD clock
  aliasing and companding.
- Integers only, Q15. 16 x 16 products use one Blackfin fractional multiply (`fmul`, with a host fallback). Each
  block of 64 samples runs in five small passes (LFO, write, read, filter, saturate and mix). No division, no 64-bit
  math, no libc in the audio path. The state is two 256-sample rings and a few words per position (about 1.1 KB
  in `.bss`, three positions); the scratch buffer is not used. `init` clears it.

## Depth (host measure, 2026-10-28)

`./a.out measure [dir]` (see Checks) runs steady sources through the plugin's C at 22.05 kHz and follows each partial
with a 45 ms sliding DFT: a 441 Hz sine, a JUNI-like saw chord (132 / 154 / 198 / 265 Hz) and its lowest saw alone.
Source-only, not native, not a listen. Figures: the wet tap's vibrato (RMS cents), the moving part of the output
against the dry (dB) and the output partials' level wobble (RMS dB), for the single saw and the chord.

| | wet vibrato | moving energy | level wobble |
|---|---|---|---|
| old default (II, DEPTH 73, MIX 64), mono, two taps | AM only (taps cancel) | -17.2 / -15.4 dB | 0.41 / 0.92 dB |
| old default, stereo (each side) | 7.6 c | -13.0 / -11.4 dB | 1.56 / 1.68 dB |
| new default (I, DEPTH 85, MIX 75), mono = stereo side | 5.4 c | -11.3 / -9.8 dB | 1.88 / 2.07 dB |
| new default at MIX 100 | 5.4 c | -8.8 / -7.3 dB | 2.56 / 2.85 dB |

The two-tap mono sum was weakest in I+II (moving energy -35 dB on the single saw: all but static); one tap gives the
stereo side's -19 dB. The wet vibrato follows the rate: I is slower than II, so at the same sweep it bends the pitch
less (I 5.4 c, II 8.5 c at DEPTH 85). The full numbers and WAVs: OP1-Emulator `run/juni-chorus-depth-2026-10-28/`.

## Screen

After the owner's mockup C, "swaying birches" (`juni_chorus-art.png`, a 1536 x 1024 image-generator PNG picked
2026-10-28; no vector exists). Five birches stand at the water line in front of a blue moon with a dashed halo,
between green cliffs and navy ranges, with red reflections on the water.

- The fit: one uniform scale, `x = 40 + art_x * 5/32`, `y = art_y * 5/32 - 4`. The art's height fills the screen, its
  3:2 width lands at x 40..280; the 4-row lift keeps the lowest reflection dash (art y 890) inside the OLED's lower
  target (y 135.1 with the stroke; it would reach 139.6 unlifted). The art's labels are blanked; the screen's are
  native text in the corners as on the other FX screens: `mode` (blue) with the mode (`1`, `2`, `1+2`; the font's
  capitals are icons, so not `I` / `II`) top left, `depth` (green) top right, `tone` (white) bottom left, `mix` (red)
  with its bar bottom right.
- `gen_svg.py` writes `juni_chorus.svg` (the static part: ranges, cliffs, horizon, clouds, stars, the marks under the
  feet; drawn through the static-prefix cache, the whole SVG static) and `juni_chorus_geom.h` (the moon, the trees,
  the reflections). Its header lists how each part comes from the PNG and every forced change.
- Movement (`juni_chorus_screen.c`, which only reads what `juni_chorus.c` writes after each block: the LFO phase and
  increment, the ramped sweep, the input peak):
  - **Sway**: every tree point moves sideways by its tree's lean x (height above the foot / tree height)^2; the foot
    stays put, leaves ride their branches. The lean is a sine of the plugin's LFO phase (the audio uses a triangle;
    the sine of the same phase reads as wind, the triangle as a metronome). Trees 1, 3, 5 and 2, 4 are 180 degrees
    apart, like the two chorus taps, with a small extra spread (14 degrees per tree) so they don't move as one.
    The lean reaches 8 px at the full sweep (DEPTH 99 in I or II), 1.6 px at DEPTH 0 (the plugin's 20 % floor), and
    a quarter in I+II (its shallow sweep), times MIX (full from MIX 50 %).
  - **Rate**: the shown sway runs at half the chorus LFO in every mode, so the trees move calmer than the sound
    (tester feedback 2026-10-28): locked to half the plugin's phase in I (0.25 Hz) and II (0.4 Hz). I+II's 9 Hz would
    alias at the screen's frame rate into a slow wave, so above 2.5 Hz of LFO the screen runs its own phase at half
    of 2.5 Hz, 1.25 Hz (fast and small, still clearly faster than II). The reflections' ripple keeps the LFO's
    own rate.
  - **Reflections**: each tree's column of dashes follows its lean, mirrored (deeper dashes follow higher up the
    tree), and every dash ripples (three waves per LFO cycle, travelling down the column) and stretches, up to 3 px
    at MIX 99 with a loud input, 0.6 px in silence; the free dashes on the water drift with it. The input level is
    read in octaves (log2 of the block peak above 16, full at 5 octaves, a peak of 512): the FX input is quiet in
    practice (a held Wavetable note peaks near 75 of 32767 in op1emu, about 600 on the attack), so a linear scale
    would never move; a held note reads as about half.
  - **TONE**: above its default (18 %) the moon dims to 40 % and its dashed halo dims to 30 % and closes in to 3 px
    off the ring at TONE 99, a darker, nearer night. Below the default the moon is as drawn.
  - **MIX 0**: no sway, no ripple: the scene is the mockup. When the effect stops running (bypass, another FX) the
    scene stops too; the stock overlay draws the bypass dimming.
- Draw cost (op1emu, executed packets per screen update, OP1-Emulator `run/juni-chorus-art-2026-10-28/profile.py`):
  silent 3.37M, playing 3.68M; entering the page, first update 4.30M (switch gap 15.7M, of which about 11.4M is the
  same for a stock FX page). Stock spring 1.40M / 1.51M, JUNI 1.85M / 1.94M, SISTER about 6.1M (its own run). At
  most 593 raster lines a frame: 80 leaves are 320 of them.
  - What it costs: every primitive, about 4-5k packets, a `raster_line` call or a segment of a live SVG element
    alike (AGG's per-path set-up), so the frame cost is the number of primitives. Tried and dropped (2026-10-28):
    the crowns as sheared SVG groups (one element per leaf: steady -9 %, entering +6 %; with diamonds: the same
    steady cost as raster lines, entering worse) and as one multi-subpath path per group (10 % slower: AGG's cost
    follows the bounding box).
  - Cut from the first version (4.02M / 4.44M, entering 4.93M): leaves as 4-line diamonds (tips and widest point)
    instead of 6-point almonds, the moon ring in 24 pieces, two chords per halo dash, trunks sampled every 5 rows,
    branches within 0.3 px, the static traced lines within 0.25 px (958 -> 587 points), 32-bit math.
- Forced changes: trunks are white lines of the measured width (cubic centre line, linear taper; the art's filled
  shapes can't bend), bark marks short black lines on them, leaves 4-line diamonds from each leaf's principal axes
  (6-point almonds read the same at 1x and cost half as much again);
  the moon's ring leaves out the pieces the art hides behind the crowns (they stay hidden as the crowns sway a few
  pixels). The clouds are measured primitives (the tracer doubles their soft outline). The reflection dashes are
  1.25 px wide, not the art's 0.9: at 0.9 the native raster draws them as a dim, broken red (seen in the first
  emulator captures), and the owner has asked for a visible red reflection.

## Checks

`juni_chorus_test.c` (host, address and undefined-behaviour sanitizers):
`cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -DOP1_HOST -I.. juni_chorus_test.c juni_chorus.c -lm`
then `./a.out check`, `./a.out render <dir>` (22.05 kHz stereo WAVs) or `./a.out measure [dir]` (the depth figures as
JSON, and short WAVs when a folder is given). It checks all 3^4 knob corners at full-scale
input for UB, silence in/out at all three positions, a tail that ends, mix 0 equal to the dry signal, and a MODE
sweep for clicks, and that mono equals the stereo left side bit for bit through a knob sweep.
`python3 harness/audio.py juni_chorus` runs the mono path only.

Measured on bcore (tier-2 guest, `run/juni-chorus/profile.py` in OP1-Emulator, worst-case knobs, instruction
packets per 64-sample call): mono render about 7.7k, stereo render about 10.2k, the same in every mode. At 345
calls per second, the CPU model fitted to the owner's unit (OP1-Emulator `docs/archive/cpu-readings-2026-10-27.md`: about
0.48 AUDIO points per 1000 packets per call) predicts about 3.7 points mono and 4.9 points stereo. Any master effect
also turns the tape-track work back on (about 5 points on rc-3.1b), so expect roughly 10 points as the master FX,
against Clouds' measured 24. A prediction, not a reading from the unit.

The screen's telemetry (a peak over every 8th input sample and four stores per block) adds about 1 % to the call
(bcore `cost_check.py`: mono 8,888 -> 8,976, stereo 13,011 -> 13,173 packets at the knob extremes); the output is
bit-identical (`exact_check.py --compare`: all 49 regimes; the host WAV renders hash the same).

The one-tap mono path (2026-10-28) drops one read pass: bcore `cost_check.py` mono 8,976 -> 7,025 packets at the knob
extremes, stereo unchanged (13,173); `cost_check --bursts` mono 5,891 per call, worst block 6,457 (selection),
stereo 10,922 / 11,490. The declared cost (`plugin.json`, the stereo master call) stays 10,922, 5.2 % of the unit's
block against the FX slot's 20 % (`slot_budget.py`). The packet figures above are from before this change.

`screen_check.py` (in `make check`): the real DSP under the screen (`juni_chorus_screen_test.c`): every state and
every sway phase inside x 0..320, y 0..139 (strokes and glyphs), MIX 0 still, the sway follows DEPTH and MODE (sign
changes in 10 s: I 5, II 8, I+II 25), the ripple the input, TONE only the moon; previews.

Emulator (OP1-Emulator `run/juni-chorus-art-2026-10-28/`, op1emu display, not the OLED): the pushed file's relocated
text equals the package's; captures of every state, an 8-frame strip while a note plays (the trees sway, the
reflections ripple), `compare.png` (the mockup at the fit beside the native frame at TONE 0, MIX 0). With a held
Wavetable note the free reflection dashes move about twice as much as in silence (pixels changing frame to frame
62 against 34, `ripple-diff.json`).

Not verified: listening on the OP-1, the real R246 decimator chain at 22.05 kHz, loading the file through the
firmware and sysmon load, and the character against a real unit. Renders in `run/juni-chorus/` are host-only
(the plugin's own rate, 22.05 kHz, with no 2:1 chain).
