# SISTER

SISTER is a stereo cathedral reverb for the original OP-1 on the kennel core, Father's sibling. Its tail can sing,
ring in tune or swell backwards. Four macro knobs move smoothly between those three characters:

- **CHOIR**: the tail runs through two slowly drifting vowel formant filters (aah, ooh, eeh), one per side, so a
  held chord comes back like a distant choir singing it.
- **PIPES**: the tail rings on tuned resonances, a root with its fifth and octave, so drums and noise turn into
  harmony, like organ pipes or sympathetic strings.
- **VESPER**: a reverse swell. The tail is read back in reversed windows, crossfaded, so each sound swells up
  backwards out of its own tail before it lands, up to 1.4 s.

Loadable plugin (`"loadable": true`, name-allocated, no order number), stereo FX entry (KN flag 4), author Hundekraft.

## Knobs as built

Every knob is a continuum; every macro moves several parameters, each smoothed per block (no zipper, no clicks on
fast turns: `check.py`).

| Encoder | Macro | 0 | 99 |
|---|---|---|---|
| Blue | SPACE | chapel: RT60 1.8 s, damping corner 7.5 kHz, pre-delay 8 ms, early taps 0.30, swell window 0.45 s | cathedral: RT60 13 s, corner 3.4 kHz, pre-delay 60 ms, early taps 0.15, swell window 1.4 s |
| Green | VOICE | CHOIR, vowels drifting gently around aah | PIPES, ringing 4 s on the root, fifth and octave |
| White | SWELL | forward tail | fully reversed (VESPER) |
| Orange | MIX | exactly dry (sample for sample once settled) | wet only, with a gentle brightness tilt |

SPACE: RT60 grows exponentially, the damping corner and early-tap level fall, pre-delay grows linearly. The
reverse window length follows it too; a new length starts with the next window, so turning it never cuts one.

VOICE, along its range:
- 0..30: CHOIR. The vowel drift widens from about a third of a vowel around aah to the whole aah / ooh / eeh
  cycle, and quickens from 0.06 to 0.16 Hz; the formants sharpen (Q 6 to 11). Left and right drift at slightly
  different rates, so the two voices sing different vowels.
- 30..70: an equal-power crossfade from CHOIR to PIPES. The plain tail comes up in the middle (0.32 to 0.57 to
  0.32), so the blend is a full hall with both colours in it.
- 30..99: the pipes' ring time grows from 1 s to 4 s and their tone brightens (loop low-pass 0.52 to 0.80).
- Level followers on both resonator banks keep them near the tail's loudness, whatever the source and the key.
  In-key material would otherwise ring up to 20 dB louder than noise.

SWELL crossfades the forward tail into the reversed windows with equal power, shortens the crossfade between
reversed windows from 60 ms to 20 ms (a sharper landing) and widens the reverse readers' stereo offset from 5 to
20 ms. The reverse ring is mono (it holds the tail and the early taps), so it gets +3.5 dB to match the forward
tail's loudness.

MIX is an equal-power dry/wet law (dry gain exactly 1.0 at 0). From about 60 up, a high shelf over a ~1.9 kHz
one-pole lifts the wet's top (up to +3.5 dB), so a fully wet tail stays present.

**The PIPES key** is green's second page: press green, then turn. One detent is one semitone, up to 24 semitones
either way of the default C3; Shift + turn moves by cents. The screen's green label reads `key c3`
there. This is the FX press-to-page mechanism HAIL uses for its pitch (`OP1_PRESS_PAGE_SEMI`, `dispatcher.c`). The
brief asked for Shift + green on the main page if the API had it; FX have no Shift-turn hook on the main page, and
this is the nearest existing one. Word 6 is the key; `page2_defaults` sets it to C3, and an unset word (0, older
sounds, a fresh master slot) also means C3. So the lowest reachable key is C#1, not C1. The key glides to a new
pitch at no more than half a sample of comb delay per block, so a key change never clicks.

## DSP

All original code; the textbook sources are listed below and in PROVENANCE.md. The FX callback runs 64 samples per
block at 44.1 kHz on the unit (the output stream's rate: OP1-Emulator `docs/emu-audio-clock-2026-10-21.md`). The
wet path runs at half that, 22.05 kHz and 32 samples per block, through a 19-tap half-band decimator and
interpolator. That halves its CPU and memory, and a cathedral tail has little above 10 kHz. `gen_tables.py`
derives every rate-dependent number from one constant (`FS`); see "Open questions" below.

```
L+R -> half-band 2:1 -> pre-delay (8..60 ms) -> 4 input allpasses ----------------------+
       \-> early taps (two per side, crossed)                                            |
4-line FDN: each line = slowly modulated read + damping one-pole + an allpass,          <-+
            Hadamard scattering, per-line decay gains (RT60 from SPACE)
tail L/R -> body + early taps + CHOIR (2 x 3-band resonators) + PIPES (3 combs) = forward wet
forward wet -> mono reverse ring (1.4 s windows) -> crossfaded reversed windows
forward / reverse blend -> x2 -> brightness tilt -> DC remover -> block limiter -> half-band 1:2 -> dry/wet mix
```

- **Tank**: four lines of 77..136 ms (1693, 2083, 2531, 3001 samples) in exact-length rings, each read with a
  slow triangle modulation (0.31..0.67 Hz, 7 samples) through linear interpolation folded into its damping
  one-pole, then a 0.45 allpass in the line. A normalised 4 x 4 Hadamard scatters them back with the decay gains.
  The tail leaves through a per-block DC remover, and the +-1 LSB rounding limit cycle of the long loops stays
  inside the tank.
- **CHOIR**: per side, three constant-peak-gain two-pole resonators at the formants of a mixed choir (aah 780 /
  1180 / 2750 Hz, ooh 340 / 720 / 2550, eeh 290 / 2250 / 2950; band gains falling with the formant). The vowel
  position follows a slow sine per side; formant frequencies interpolate on a log scale, and the pole radius
  comes from the bandwidth (exp(-pi BW / FI), FI = 22.05 kHz). A quiet gate clears the resonators when the tail is silent.
- **PIPES**: three feedback combs, root, just fifth (2/3 of the delay) and octave (1/2), each with a fractional
  delay (linear interpolation) and a loop low-pass, decay gain g = 2^(-delay x log2(1000) / (FS x T60)), input
  scaled by sqrt(1 - g) so noise in gives noise out. The root sits on C3 at key 0; panned root centre, fifth
  left, octave right.
- **VESPER**: a 64,891-sample mono ring of the forward wet (2.9 s). A reader starts at the newest sample at each
  window start and runs backwards for one window (age 0 to 2 x window), so a decaying tail plays as a swell. The
  next window crossfades in with equal power while the previous one fades out; the right channel reads a few
  milliseconds older.
- **Output**: the wet gets +6 dB (the tank leaves at half scale), then the MIX tilt, a per-block DC remover and a
  block limiter (it ramps down within the block to what the block's peak needs, then releases slowly). A wet tail
  at its last two bits is set to exact silence, so a tail ends in zeros.
- **Rounding**: products in recursions round toward zero or are kept inside a loop that removes DC; the
  resonators and combs round toward zero, so a ring ends in silence. `sister_test.c` checks that every tail ends in
  exact zeros at the knob ends.

Integers only, no division per sample (per block only: the level followers, the key), no libc, no floats. State
and workspace: 173,236 bytes of the 176,460-byte FX scratch per position (`_Static_assert` in `sister.c`). No
`op1_fx_big` lease.

**FX select**: `init` runs in the audio callback, so it clears only the scalar state and workspace (about 2 KB).
The rings (171 KB) are cleared 4,096 words per block over the next 11 blocks (16 ms) while the wet path stays
silent and the dry follows the mix law (Nostromo's approach, no `fx_spread.h`). The init block is about 5.9k bcore
packets.

**Blackfin kernels** (`sister_asm.S`): the tank line, the scattering, the formant band, the comb, the allpass,
the half-band decimator and interpolator, and the mix. Each uses the Blackfin's 16 x 16 multiply-accumulate,
circular buffers and parallel loads. `sister_kern.c` holds their C references with the same integer results (the
host build uses those). `budget_check.py --cflags=-DSI_NO_ASM` builds the references for the Blackfin; on bcore the
two give identical output hashes in all 18 regimes. No kernel relies on the hardware's RND_MOD default rounding,
which could differ on ties between bcore and the unit.

## CPU (bcore packets per FX call, `budget_check.py`, 1500 blocks)

| Regime | Synth FX (mono) | Master (stereo) |
|---|---|---|
| defaults | 16.2k | 16.7k |
| heaviest steady (VOICE blend, SWELL 99, SPACE 99) | 22.9k | 23.4k |
| every knob and the key moving every block | 22.3k | 22.8k |
| worst single block, any regime | 27.1k | 27.6k |
| init block (select) | 5.8k | 5.9k |

Predicted on the unit (10.1 + 0.478 per 1,000 packets per block for the FX alone; FX on the owner's 6-voice
Supersaw base of 36 %): defaults about +8.0 points (Father measured +18), the heaviest steady regime about +11.2
points, the worst block about +13 points (49 % of the block on that base). bcore counts packets, not cycles: no SDRAM or cache stalls, no
hardware acceptance. A unit reading is open.

**L1 pool and cold lines (2026-10-30, branch `opt-sister`).** On the unit the worst FX block is the one where SISTER's
code and data are cold in the cache, so what counts is the distinct 32-byte lines a call touches. The pool partition
(`op1_l1.h`) holds, per instance: the block workspace (`si_ws`, including the decimator and interpolator buffers;
only their history stays in the state), the block parameters cached under the smoothed macros (`si_par`: recomputed
out of line only when a macro moved, marked invalid when the partition is fresh) and the tables a block reads
(`si_l1tab`, copied in when fresh): 5,376 B of the FX partition's 8,320. At the master position (4,160 B) it claims
the workspace and parameters only (1,888 B). The knob cache and the screen telemetry share one 32-byte line per
position (`sister_pos`). Traced bcore at the defaults, L1 on: data lines per call 156 -> 113, code lines 214 -> 188,
packets in the worst block 17.8k -> 17.0k (a window edge no longer runs the per-sample VESPER loop while VESPER is
silent). Left in SDRAM: the rings (pre-delay, diffusers, tank allpasses and lines: about 85 lines per call) and the
scalar state (15 lines). Audio is bit-identical (exact_check with the pool off, on and poisoned).

## Screen

Drawn after the owner's chosen sketch (OP1-Emulator `run/sister-2026-10-27/nun.png`, a 1536 x 1024 image-generator
mockup; no vector exists), traced and measured as `docs/screen-art.md` says. The fit is one uniform scale, chosen
once: screen x = 40 + sketch x * 5/32, screen y = sketch y * 5/32 - 9. The sketch's height fills the 160 rows, its
3:2 width lands at x 40..280, and the 9-row lift puts the art band (arch tops to the floor's front) at y 2.9..136.2,
inside the OLED's lower target with strokes. The sketch's labels are native text at the corner layout of the other
FX screens (space with its value, mix with its bar, voice or `key c3`, swell), in the bands the 3:2 -> 2:1 fit
leaves; the mockup's SHIMMER / TONE are the real VOICE / SWELL. `gen_svg.py`'s header lists every measurement.

A gothic nave: four pairs of slim columns with capitals and bases recede to a vanishing point; pointed transverse
arches spring from them and nest toward the centre; a pointed lancet stands on each wall between neighbouring pairs;
a tiled floor runs to the front. SPACE sets the depth step between the pairs (0.17 .. 0.64; 0.34, the sketch's, at
the default 60), so the nave deepens; the floor is the same plane, so the plinths stay on it. The rose window sits
behind the nun's head like a halo: a white ring, twelve green petal lobes, a white quatrefoil. Silent, the petals are
the sketch's lobes; with the wet level they light toward a paler glow and sing the drifting vowels (aah tall, ooh
round, eeh wide, alternate petals for the left and right voices); toward PIPES they become pipes of the root, fifth
and octave, both drawn across the crossfade. They go from 80 % green at VOICE 0 to fully green at 99, 92 %
bright when silent (the earlier screen's petals sat at 27 % and read too dim). The nun: veil, oval face with a band
across the forehead, shoulders flowing out to the hem, a necklace V and a cross. Three candles of falling height
whose teardrop flames are the sketch's size at SWELL 0, twice that at 99 (VESPER), and flicker with the tail. MIX is
the red bar.

What still differs from the sketch, and why: its perspective isn't one camera (the floor lines converge a little
higher, the arch apexes recede toward their own point). The model keeps the columns and floor on one vanishing point
and fits the apexes separately, so each part can move with SPACE. The floor is drawn symmetric (the sketch's left
front is cut shorter than its right). Sub-pixel details are left out: the three ribs on the near capital, the hub
circle behind the petals, the innermost arch's whitish run outside the ring. Measured against the trace at the
defaults (host frame, `art_compare.py`): placement median 0.28 px, 95 % within 2.0 px, 86.9 % within 1 px; the
screen-art bar for a straight PNG trace (95 % under 0.5 px) isn't the target for a modelled screen whose parts move.

Level response (screen side only; the audio is bit-identical: the four audio objects are byte-identical and
`budget_check.py`'s bcore output hashes and packets match in all 18 regimes, parent
`run/sister-2026-10-28/audio-hash-{before,after}.txt`). What the telemetry measures natively: `sister_pos[pos].peak` is
the wet path's block peak (|sample| of the 22.05 kHz wet block after the DC remover, 0 below 5) and `.level` its
1/8-per-block follower (`sister_pos`, one 32-byte line per position with the knob conversion cache; until
2026-10-30 these were the arrays `sister_peak` / `sister_level`). In op1emu a held Wavetable C4 at the defaults reaches SISTER's input at peaks of only ~60..90
of 32767 (601 at the attack; the input buffer dumped at `stereo()`, `run/sister-2026-10-28/inprobe.py`), so the wet
block peak is ~5..30, ten octaves under the host model's 20000; the old `lvl * 4` mapping lit the petals by under
1 %. The screen now follows the block peak in octaves above 4 (an integer log2 from the leading bit plus 8 mantissa
bits) with three followers: fast (~40 ms up, ~120 ms down), slow (~60 ms up, ~700 ms down) and an even ~300 ms one.
The slow one lights the petals, sets how far they sing and how much the flames flicker (all of it at 6 octaves,
peak 256); the fast one's lead over the even one makes the petals breathe (1 px per octave, at most 1.2 px) with the
playing at any loudness. Silence returns all of them to 0: the still sketch. Native (op1emu, `strip.py`): a held
Wavetable C-E-G chord differs from the silent frame in 450..740 pixels by more than 30 (max 320) and moves frame to
frame (20..250 pixels per 3 frames); a held C4 in 555..611; silent frames are identical to each other. The strip is
in parent `run/sister-2026-10-28/screen/strip-v2/`.

Draw cost: native op1emu, executed packets per screen update (stock update entry to end, Wavetable + SISTER, silent
then a held note; packets, not hardware time). The slimming pass (2026-10-28, parent `run/sister-slim-2026-10-28/`):
silent 5.58M -> 2.92M, held note 6.13M -> 2.92M, returning to the page (cache warm) 7.5M / 6.8M -> 4.6M / 4.1M, the
first visit (the static art captured) 9.95M -> 7.7M. Where it went before: the black fills under the rose and the
nun (54 wide lines, 1.6M), the ring, quatrefoil and nun outline (177 lines, 1.0M), the nave (240 lines, 1.7M), the
petals (110, 0.54M); a 1 px line costs about 3k packets plus 0.45k per pixel of length, a wide fill row 28..32k.
The ring, quatrefoil and nun outline are now in the SVG (a cached pixel copy), the fills are gone (the live parts
are clipped instead), the petal heads are 60-degree arcs (5 points, was 9), the flames 5 points a side (was 7), the
arch profile leaves out its springing point (0.33 px at most): 318 live lines at the defaults, was 619. Native
captures old | new differ only in anti-aliased edge pixels (parent `compare.png`). Earlier: median 9.03M before the
2026-10-28 redraw, 6.11M after it. The audio path is untouched.

Every moving part eases per frame toward its target (~110 ms, flames ~45 ms). In op1emu at the default link rate
guest time runs 2x against audio, so the eased motion in native frames looks twice as fast as it will on the unit.
The vowels ease the short way round their cycle.

Everything still is the SVG, drawn through the static-prefix cache with `first_live` NULL (the whole SVG is static): the floor, the candle bodies and wicks, the rose's ring and quatrefoil, the nun's outline. `gen_svg.py` cuts the floor
and the ring where they run behind the nun, so no black fill is needed. Everything that moves is drawn live after it,
in order: arches, lancets and columns far to near, the petals (or pipes), flames, bar, labels. The live parts are
clipped where the still art used to cover them: the nave stops at the ring's outer edge (where the rose's black disc
hid it), the petals at its inner edge, both half a pixel outside the nun's outline (`cline` in `sister_screen.c`:
2 px steps, each visible stretch one line, its ends found by bisection; `screen_check.py`'s clip run checks every
line end). `gen_svg.py` writes `sister.svg` and `sister_geom.h` together.

## Checks

- `check.py` (in `make check`): `sister_test.c` under ASan/UBSan (MIX 0 exactly dry; silence stays silent; every
  tail ends in exact zeros; 64 corners on full-scale input; positions independent; cached knob conversion),
  clicks on held chords and drums with every knob jumped and swept and the key stepped (the detector of
  `mbira_check.py`), the pipes' partials on the root and fifth harmonics within 0.6 % at three keys, the reversed
  tail rising, loudness over VOICE x SWELL within 9 dB, stereo decorrelation. Source-only.
- `screen_check.py` (in `make check`): states, eased motion after jumps, flame flicker, every stroke and glyph
  inside y 0..139; previews.
- `budget_check.py` (not in `make check`, about a minute): the CPU table above, and the asm/C exactness with
  `--cflags=-DSI_NO_ASM`. `--cflags=-DSI_PROF` adds packets per stage.
- `golden.py`: `harness/golden/fx_sister.png`.
- `presets.py OUT`: four stock-Phase sounds (choir, pipes, vesper, nave) as `sister-presets.zip`.

## Open questions

- The FX rate. This plugin assumes 44.1 kHz FX samples (the output stream's rate per the clock investigation),
  where the emulator's audio clock model says 22.05 kHz. If the unit runs FX at 22.05 kHz, the pipes sound an
  octave lower than the key shows, the formants sit an octave low, and the tails and swells are twice as long.
  Changing `FS` in `gen_tables.py` and regenerating fixes it. In op1emu the native check passed: in one reading at
  44.1 kHz an Iter C4 sits at 261.6 Hz and the wet-only pipes ring on C3 (+35 dB over the median), not C2 (-6 dB)
  (OP1-Emulator `run/sister-2026-10-27/native-pipes-rate-check.json`). A line-out recording on the unit is still open.
- The wet path's level, the choir and pipe balances against the plain tail, and the default knob positions were
  set from measurements, not by ear. They need the owner's listening.

## References (no code copied)

- J. O. Smith, *Physical Audio Signal Processing*: feedback delay networks, comb and allpass filters, the
  constant-peak-gain two-pole resonator.
- G. E. Peterson and H. L. Barney, "Control Methods Used in a Study of the Vowels", JASA 24 (1952): vowel
  formant frequencies (rounded and blended toward a mixed choir here).
