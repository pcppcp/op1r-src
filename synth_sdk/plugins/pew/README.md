# Pew

A drawbar organ through a rotary speaker, on four macro knobs (redesign 2026-10-29, branch `pew-redesign`). `pew.c`
has the sound, `pew_macro.h` the knob maps the sound and the screen share, `screen.c` the screen.

## Knobs

| Knob | Page 1 | Page 2 (press) |
|---|---|---|
| Blue | REGISTER: 008000000 -> 888000000 -> 888800000 -> 888808008 -> 888888888, in whole drawbar steps | push the low bars (16', 5 1/3', 8') out |
| Green | PERCUSSION: off, soft 2nd (slow decay), sharp 3rd (fast decay), then more key click; single trigger | push the middle bars (4', 2 2/3', 2') out |
| White | DRIVE / CHORUS: clean, scanner vibrato on the upper bars up to a chorus, then tube drive up to a growl | push the high bars (1 3/5', 1 1/3', 1') out |
| Red | LESLIE: brake / slow / fast in three zones of the knob, the rotors spinning up and down as before | none: red and white share preset word 7 in the core |

Saved PEW sounds play differently (the knobs mean other things now); the owner accepted that for the redesign.

## Sound

Three shared wave tables of one 16' period (512 points): low (16', 5 1/3', 8') through the drum, upper (the other six
bars) and percussion (4' and 2 2/3' sines) through the horn, which wobbles in pitch (Leslie Doppler plus the scanner
vibrato). The tables carry the sounding level and a loudness compensation ((E_ref / E)^(1/4), 0.8 .. 1.3), so a thin
registration is not much quieter than a full one. They are rebuilt only when a drawbar step or the percussion's
harmonic changes, 16 points a block into a second copy that then takes over (about 95 ms from a step to its sound;
64 points a block, 23 ms, when a phrase starts with no key held, as after loading a preset; the first tables are
built whole at boot, from the screen's load). Blue moves in drawbar steps, so an LFO belongs on green or white.
The key click is one noise burst for all keys, rebuilt 64 samples a block when its level steps.

On the OP-1 the sample loop runs as two hand-scheduled passes in `pew_dsp.S` (host builds keep the C loop in `pew.c`):

- `pew_osc` (23 packets a sample): phases, the horn's pitch wobble as one 16 x 16 multiply, the low and the paired
  upper | percussion reads, the percussion level, and the word hi << 16 | lo.
- `pew_out` (11, 12 with the click): drum and horn tremolo, the drive's input gain (the sum taken >> 14), the knees
  (passed in: 24,000 down to 11,000 / -8,000 with the drive), the output gain, saturated as `engine_sat16`.

Checks after any change to `pew.c` or `pew_dsp.S`:

    python3 synth_sdk/plugins/pew/kern_check.py        (kernels against the C loop on bcore, bit for bit)
    python3 synth_sdk/harness/cost_check.py --bursts pew

Cost (bcore packets per voice call, six voices at C7..C8): 6,428 -> 5,776. Predicted worst block with the budget-v2
fit (3.9 x packets + 115 x fills + 90 x write misses, cold): six held voices 24.4 %, chord re-strikes 26.5 %, a knob
sweep every block 29.9 % (the old PEW: 37.8 %). Root `run/pew-redesign-2026-10-29/cost/`.

## Screen

`gen_svg.py` traces `pew-art.png` (the owner's concept 3, "jazz club") into `pew.svg` and `pew_geom.h`: one uniform
scale limited by the height (0.187, ink y 3..138), then the side groups (window and table, Leslie and plant) moved out
whole to the screen's edges as JUNI's, the mockup's colours mapped to the stock and encoder colours (Oxbow's ochre for
the wood). The scene is still and drawn once through the core's
static-prefix cache; the three green notes are the SVG's only live nodes (after the boundary). `screen.c` draws on
top: the drawbars (the registration the sound plays), the notes flying up at each strike (green), the smoke and the
lamp's glow (white), the Leslie's red lines turning with the audio's rotors (red), and a lit key per held note.
Regenerate with `python3 synth_sdk/plugins/pew/gen_svg.py` (numpy, scipy, PIL; about 13 s).
