# rackverb

Late-80s rack reverbs and effects for the OP-1, as a stereo kennel FX (loadable, no order number, author Hundekraft).
Four units, each with its full program list:

| Unit | Programs | In the style of | Engine |
|---|---|---|---|
| VERB I | 1..63: 50 reverbs (0.2..20 s), 9 gated, 4 reverse | Alesis Midiverb (1986) | the machine at 23,437.5 Hz |
| VERB II | 00..99: 29 reverbs, 10 gated, 10 reverse, 10 flange, 10 chorus, 20 delays, 10 EFX (00 = defeat) | Midiverb II (1987) | the machine at 31,250 Hz, LFO-patched reads |
| FEX | 1..63: echoes, 2/3-tap delays, regenerating echoes, slaps, reverbs, multi-taps, thickeners, stereo generators | MidiFex (1986) | the machine at 23,437.5 Hz |
| QUAD | 1..90 in five configurations (EQ > pitch > delay > reverb, Leslie > delay > reverb, graphic EQ > delay, 5-band EQ > pitch > delay, EQ > reverb) | Quadraverb (1989) | rv_quad.c at 44,100 Hz, full band |

Design, research, the program tables and the licence position: OP1-Emulator `docs/midiverb-design-2026-11-04.md`.
The owner's decisions (2026-11-04): our own names (no Alesis names or logos), VERB II at 31.25 kHz, QUAD as the
fourth unit (12 % CPU aimed for, up to 15-20 % allowed), 90 QUAD presets of our own, a quick fade on a program change,
the mockup's screen plus a level bar, SIZE on blue's page 2, default VERB I 14, mono in and stereo out.

## Knobs

| Encoder | Turn | Press, then turn (page 2) |
|---|---|---|
| Blue | FEEDBACK 0..99 %: the wet sum back into the input, through CUTOFF; the top is capped per program shape (a reverse or a bloom runs away sooner than an echo) | SIZE 50..100 %: every delay of the program shorter (and its decay with it); 0 (an unset word) means 100 % |
| Green | CUTOFF 500 Hz..11 kHz, a one-pole on the wet at the unit's rate | MODE AUTH / CLEAN, one detent each |
| White | MIX dry..wet, equal power, exactly dry at 0 | UNIT VERB I / VERB II / FEX / QUAD, one detent each |
| Orange | PROGRAM, one per detent (`OP1_FX_STEPS(100)`); it turns round within the unit's list: past the last program to the first, below the first to the last (`op1_fx_wrap[pos][3]` = the unit's count, set in render; owner 2026-11-06) | none |

The wrap needs a kennel core from 2026-11-06 (an older core skips the file: it lacks the `_op1_fx_wrap` import). Orange and the two stepped page-2 knobs need a kennel core from 2026-11-04 (dispatcher.c: stepped FX knobs). An older
core turns orange in its fine steps (about 12 detents over the range) and leaves MODE and UNIT where they are.

A program, unit or mode change fades the wet out over one call (1.45 ms), builds the new program and clears the ring
1,024 words a call while the dry plays on (VERB / FEX: 8 calls, 12 ms; QUAD: 8 KB a call up to its work area, 26 calls, 38 ms), then fades the wet in.

## The machine (VERB I, VERB II, FEX)

`rv_mv.c` models the Midiverb-I-class DSP from its published description (Eric Brombaugh's MIDIVerb_RE slides, MIT;
thement's midiverb_emulator, GPL-3.0): four operations (SUMHLF acc += m/2, LDHLF acc = m/2, STRPOS m = acc and
acc *= 1.5, STRNEG m = ~acc and acc = ~acc/2), 128 steps a sample, a 16 K x 16-bit ring that every step walks one word
a sample, the ADC word written at step 0, the DAC words read at steps 0x60 and 0x70. No ROM or ROM-derived program is
used: `gen_programs.py` turns each program's name into a row (`rv_programs.h`) and the generator in `rv_mv.c` writes our
own 128-step program from it: input allpasses (g = 0.5) into a two-half loop with 0.5 low-passes for reverbs, a tap
cloud over a window for gated / reverse / bloom, a regenerating line with 0.5 filters and stereo taps for echoes,
quarter-sample modulated reads for chorus and flange (the LFO rewrites their addresses every call, as the Midiverb
II's controller patched its program RAM), a complementary comb pair for the stereo generators.

Around it at 44.1 kHz: a 2-pole analog low-pass, linear resampling to the machine's rate, the converters (AUTH: a
12-bit ADC shifted into the word and a 13-bit DAC; CLEAN: 14 and 15 bits), FEEDBACK and CUTOFF at the machine's rate,
back to 44.1 kHz and the same low-pass (AUTH 10 kHz, CLEAN 11 kHz for VERB I / FEX, 14 kHz for VERB II).

**The block engine.** Interpreting 128 steps for each of 34..46 samples a call costs too much on the Blackfin. Every
step's address moves one word a sample, so the engine runs one step over all the call's samples at a time
(`rv_mv_block`, inner loops two samples at a time in `rv_kern.S`). That is exact as long as no read reaches back fewer
samples than a pass is long, except reads of words written earlier in the same pass: a call is cut into passes no
longer than the program's shortest delay (`prog.chunk`), the 0.5 low-pass groups (a one-sample recurrence) run as one
loop, the lines are 48 words apart in the ring, and a pass starts with a load. `rv_mv_run_ref` is the plain
one-sample-at-a-time machine; `rv_test.c` (host) and `kern_check.py` (the Blackfin build on bcore) compare them bit
for bit, the ring included. Flanges keep their sweep's floor at 16 samples (0.5 ms) so they run block-wise too.

## QUAD

`rv_quad.c`: EQ (biquads), pitch (chorus, flange, detune, phaser), Leslie, delay (mono, stereo, ping-pong) and reverb
(plate, room, chamber, hall on a figure-eight tank; reverse; optional gate), 90 presets of our own
(`gen_quad_presets.py`), every stage at the FX call's 44,100 Hz (owner, 2026-11-04: full rate), its heavy loops in
`rv_quad_kern.S` (bit-exact with the C on bcore: `quad_kern_check.py`). Its 212 KB of rings and state live in the position's fast
lease in heap 0 (`op1_fx_fast`, kennel cores from 2026-11-06), else in its big buffer (`op1_fx_big`, the pool), cleared 8 KB a call on a preset change (26 calls, 38 ms dry); its per-call work area
(`qd_work`) is the caller's: rackverb.c hands it the L1 pool's partition (see Cost). Its own header has the details.

## Screen

The owner's approved concept A, revision 2 (OP1-Emulator `docs/design/rackverb-v2/a-v2.svg`, 2026-11-06), built at its
own coordinates. `rackverb.svg` (`gen_svg.py`): the frame with rack ears and screws, the LED window, the knob column
lines, each knob's ring and seven ticks in its encoder colour, the strip's line, and per unit a logo group (the name
in bold slanted stroke letters, three blue stripes after it). `rackverb_screen.c` draws the program number as two
7-segment digits skewed 6 degrees (unlit segments dim red), the program's name (shrunk to fit x 18.5..112 when long,
never scrolled), each knob's label, pointer (135..405 degrees) and value by its own page (FEEDBACK / SIZE, CUTOFF /
MODE, MIX / UNIT; a pushed knob shows its page-1 value small in grey under the value), and the strip: PROG n/count, a
stereo L / R meter (two rows of 20 steps of 2 dB, the top 3 red, unlit steps dim) and the AUTH / CLEAN tag. The meter
reads the output's peaks after the mix (`rv_live[p][4]` L, `[5]` R; nothing rendered changes); at a mono position
(synth, drum) both rows show the one output.

## Files

| File | What |
|---|---|
| `rackverb.c` | the FX: knobs, program changes, resampling, converters, FEEDBACK, CUTOFF, MIX, QUAD's state lease |
| `rv_mv.c`, `rv_mv.h` | the machine (reference, branch-free and block kernels) and the program generator |
| `rv_kern.S` | the block engine's four steps, two samples at a time |
| `rv_programs.h`, `gen_programs.py` | the program rows of VERB I, VERB II and FEX, from their names |
| `rv_tables.h`, `gen_tables.py` | CUTOFF, MIX, the analog low-pass |
| `rv_quad.c`, `rv_quad.h`, `rv_quad_presets.h`, `gen_quad_presets.py` | QUAD |
| `rv_quad_kern.S` | QUAD's Blackfin kernels (empty on the host) |
| `rv_fbceil.h` | FEEDBACK's ceiling per program, measured (`rackverb_host fbceil`, built with `-DRV_FBCEIL_SEARCH`) |
| `rackverb_screen.c`, `rackverb.svg`, `gen_svg.py` | the screen |
| `rv_test.c` | host: every program builds, fits, is stable, falls silent; reverb decay near its name; kernels bit-exact |
| `rackverb_host.c` | host: the whole FX (every program of every unit, the dry while clearing, MIX 0 exact) and a WAV renderer |
| `quad_test.c` | host: QUAD's 90 presets |
| `kern_check.py`, `kern_guest.c` | bcore: the Blackfin block engine against the reference |
| `quad_kern_check.py`, `quad_kern_guest.c` | bcore: QUAD's kernels and the whole qd_render against its C (`--cost`: packets per configuration) |
| `quad_cpu.py`, `quad_cpu_guest.c` | bcore with the calibrated cache / SDRAM model: the whole FX's worst block per program as the unit's %, per unit and QUAD configuration; `--free` / `--free-sym` cost a range, `--json` / `--compare` output hashes before and after a change |

## Checks

```
cc -O2 -Wall -DOP1_HOST -I../.. -o /tmp/rv_test rv_test.c rv_mv.c -lm && /tmp/rv_test
cc -O2 -Wall -I../.. -I. -o /tmp/rv_host rackverb_host.c rv_mv.c rv_quad.c -lm && /tmp/rv_host check
python3 synth_sdk/plugins/rackverb/kern_check.py          # from the kennel checkout
python3 synth_sdk/harness/plugin_cost.py rackverb         # bcore packets per call
python3 synth_sdk/plugins/rackverb/quad_cpu.py --units 3,0,1,2   # the unit's worst block per program (stall model)
```

## Cost

The unit's worst block, predicted with bcore's calibrated cache / SDRAM model (`quad_cpu.py`: the whole FX at the
master, cold caches every block as the calibration found on the unit, the scratch at the master's address, QUAD's
state in the master's big-buffer lease; own share = cycles / 5,805 %, kennel ARCHITECTURE "Testing"), a C-E-G chord
of saws feeding it, every program, 300 steady blocks after each program change and the change's own blocks. The
owner's slot: each FX 12 %, QUAD up to 20 %. 2026-11-06, branch `rackverb-quad-cpu` (OP1-Emulator
`run/rackverb-quad-cpu-2026-11-06/`):

| Unit | Worst block before -> after | Average, worst program | Worst program (display number) |
|---|---|---|---|
| QUAD config 1 (EQ > pitch > delay > reverb) | 25.2 -> 16.0 % | 23.2 -> 15.1 % | 10 |
| QUAD config 2 (Leslie > delay > reverb) | 24.0 -> 15.2 % | 22.0 -> 14.6 % | 37 |
| QUAD config 3 (graphic EQ > delay) | 9.8 -> 5.9 % (a clearing call 8.0 %) | 9.4 -> 5.9 % | 46 / 54 |
| QUAD config 4 (5-band EQ > pitch > delay) | 11.9 -> 7.5 % (a clearing call 8.0 %) | 11.5 -> 7.3 % | 60 |
| QUAD config 5 (EQ > reverb) | 21.4 -> 13.5 % | 19.6 -> 13.0 % | 71 / 74 |
| VERB I | 13.6 -> 11.7 % | 12.8 -> 11.0 % | 24 |
| VERB II | 14.6 -> 12.7 % | 13.9 -> 12.0 % | 14 |
| FEX | 13.1 -> 11.3 % | 12.8 -> 11.0 % | 49 |

With SIZE swinging between its ends every 64 blocks: QUAD config 1 26.2 -> 16.6 %, VERB I / II / FEX 15.4 / 16.3 /
15.1 -> 13.7 / 14.4 / 13.3 % (a SIZE change rebuilds the machine's addresses in that block). A program change is
not a spike: the change's blocks (hold, fade, the clear 8 KB a call, the build) stay under the steady worst.

What made QUAD's blocks heavy: memory, not arithmetic (packets alone are 6.1 % for config 1). Its per-call work area
(the rows, the tap spans, the tank rows, the kernel argument blocks, 4.2 KB) sat in the big buffer after the rings:
cold every block, and each store to a line not in the cache is a single SDRAM write (no write-allocate). Freeing
ranges in the model, config 1: the work area 5.8 points, the rows 2.2, the wrapper's rows 2.0, the code (instruction
cache) about 6, the rings about 6. The fix: the call's rows (`rv_rows`, 3,840 B: VERB / FEX the resampler's rows,
QUAD its `qd_work`) live in the L1 pool's partition (`op1_l1_claim` every call; FX 8,320 B, MASTER 4,160 B), else in
their home in rv_state; the tank's wrap copies and the output taps' spans share memory (never live at once); the
work area's 7th tap row replaced the unused 8th. Output bit for bit the same as before on bcore (every program of
every unit, SIZE still and swinging, L1 on, off and with junk in the partition every call; `quad_cpu.py --compare`,
`exact_check.py` with `T2_L1` 0 / 1 / poison, `quad_kern_check.py`, `kern_check.py`), with one timing change: QUAD's
clear stops at its work area, so a QUAD program change clears in 26 calls instead of 27 (the wet comes in 1.45 ms
sooner).

What is left in VERB II's 12.7 % (model, freed one at a time): the code 3 points, the ring 3, the stack 0.8, the
program 0.6, the wrapper's state 0.3, packets 6.3. Next steps if the unit needs them: the program and the wrapper's
state in L1 as a state lease (`op1_l1_claim_state`; QUAD's rows leave no room for them at the master, so the
lease would change size by unit), the wrapper's biquads in asm (about 25 packets a sample now, x 192).

SDRAM banks (2026-11-06, branch `rackverb-cpu2`, OP1-Emulator `run/rackverb-cpu2-2026-11-06/`): the model prices
every fill as bank 0's (its calibration ran on the heap-0 arena), but QUAD's state sat in the pool (`op1_fx_big`,
bank 2, framebuffer A's bank), where K1b measured a clean fill at 354 cycles against bank 0's 193. `quad_cpu.py
--bank2` runs each job again with the pool counted as L1 and adds the pool's stalls x 0.83 more (an approximation:
per-program maxima, K1b's clean-fill ratio). QUAD now takes the core's fast lease in heap 0 (`op1_fx_fast`, 224 KiB a
position, 688 KiB of heap 0 for the three, taken at boot only when a loaded file imports it) and falls back to
`op1_fx_big` when the core has none. Worst block, own share of the unit's block:

| Unit | Model (bank-0 pricing) | Bank-2 view, state in the pool | Bank-2 view, state in heap 0 |
|---|---|---|---|
| QUAD config 1 | 16.1 % | 20.1 % | **16.1 %** |
| QUAD config 2 | 15.3 | 18.9 | **15.3** |
| QUAD config 3 | 6.2 | 6.6 | **6.2** |
| QUAD config 4 | 7.8 | 8.5 | **7.8** |
| QUAD config 5 | 13.5 | 17.3 | **13.5** |
| VERB I / VERB II / FEX | 11.9 / 12.7 / 11.5 | the same | the same |

So the unit should read about 4 points less with QUAD configs 1, 2 and 5 than with the L1-rows build (b0f8c16); the
model's own column is unchanged (+-0.2 from code placement). Output bit for bit the same in both places
(`quad_cpu.py --compare`, the default and `--no-fast`, 314 programs each). What is left in config 1's 16.1 % (freed
one at a time, 2026-11-06): the code 4.5 points (render 1.0, qd_render_w 2.4, the kernels 0.5: compulsory fills of
the ~8 KB the block runs; `-Os` was bit-exact but 0.2..0.8 points worse), the rings 4.6 (bank-0 pricing), the stack 1.3
(the callback runs on a thread's heap-0 stack in the release build; the kernels' register saves and qd_render_w's
frame, evicted by the ring spans), the tables 0.7, the plugin's `.bss` 0.4, packets about 4.5.

Unit reading before (kennel dab7618, the owner): Moai alone 41 %, with QUAD on the master 60 % and crackle (red bar);
MONOLIT 37 % alone, crackle with QUAD. The model's QUAD average for configs 1 / 2 / 5 then was 20..23 %.

`plugin_cost.py` packets per call (2026-11-04, its guest has no big-buffer pool, so QUAD runs dry there): VERB I 14
(default) 27,047 average, 28,941 worst at the synth position; the master 28,239 / 30,147. QUAD's own packets per
qd_render call, worst preset per configuration (`quad_kern_check.py --cost`, 2026-11-06): 27,910 / 24,160 / 8,324 /
12,404 / 17,763.
