# op1r contribution update — 1.2.4

- Bring in the published 1.2.4 visual changes: stable subpixel star paths,
  soft sun/edge fades, a separate 37.5% small-star drift clock, and five new
  green cross/asterisk signatures alongside the five retained green outlines.
- Extend host coverage for star continuity, solid-sun occlusion and drift ratio.
- Preserve the op1r adapter, native packer and host harness compatibility.
- Audio, blue wave geometry, controls and all 15 contribution presets are
  unchanged, including Stone Hall saved dry at octave -3.
- See TESTING.md for current build checks and historical 1.2.3 device readings.

# op1r contribution

- Adapt the 1.2.3 sources to the current `.op1p` build and host harnesses.
- Save Stone Hall dry (T3 off), preserving sound words, ADSR and octave -3.
  The previous 1.2.3 Hundekraft release notes below describe its Plate setting.

# 1.2.3 — output level and Stone Hall Plate

- Raise fixed voice output gain from 3840/32768 to 11520/32768: 3x amplitude,
  approximately +9.54 dB. No compression, limiting or automatic gain changes.
- Preserve tables, filter/morph behavior, velocity response, envelopes, visuals
  and parameter mappings.
- Preset addendum: Stone Hall saves Plate enabled with Decay 99, Pre 99,
  Tone 00 and Mix 33, with its starting octave saved as -3. Replace only its AIF; the other 14 presets are unchanged.
- Replace the overly conservative six-identical-copies headroom assumption
  with actual six-voice sums. Add 13,440 full-envelope cases covering all
  Characters/Intervals, Morph positions, Thickness endpoints, registers,
  Motion settings, phase seeds, chords, clusters and repeated-note unisons.
- Check level matching and downstream FX/tape gain on the physical OP-1.
  No compression, reverb DSP changes or GIF updates are included.

# 1.2.2 — local evaluation: slower held return

- Holding broadens the ripple's trailing return up to twice its width and
  softens its small rebound from 1/8 toward 1/16 strength.
- Preserve the sharp leading edge, outward speed, quick tap behavior and the
  existing 1.75-second maximum lifetime. No continuous held-note oscillation.
- Audio, presets, Character sound names and all other visuals remain unchanged.
- Local only; public publication stays paused and GIFs remain untouched.

# 1.2.1 — local evaluation: hold-sensitive ripples

- Tap keeps the short outward ripple. Holding any note from that hit/chord
  earns a small fade extension, capped at 375ms: about 1.4s to 1.75s total.
- The same front travels farther while fading; holding never retriggers or
  freezes it. Partial chord release, voice reuse and indefinite holds are bounded.
- Add visual-only held-note serial tracking through the SDK note-off callback;
  audio rendering, envelopes, saved mappings and presets remain unchanged.
- Character readout uses the sound-family names (Warm, Swarm, Breath, etc.);
  its mapped wave style is unchanged.
- Evaluation stays local. Public 1.2 publication is paused; no GIF updates.

# 1.2 — waves, stars and sun each get their own controls

- Character readout retains the sound-family names (Warm, Swarm, Breath, etc.).
- Character selects 14 wave styles alongside the existing 14 sound families;
  Morph adjusts the chosen wave's movement. Blue push switches Morph/Character.
- Each key or grouped chord creates a sharp, randomly placed ripple that spreads
  outward and fades, with a small trailing wake and no persistent audio bending.
- Lower side joints move much farther; the right outline sits 8 pixels inward.
- Shift toggles the complete bottom dial/readout, starting hidden. Its black
  backing exists only while shown; hidden waves continue below the old cutoff.
- Interval selects ten distinct added star fields and signature-star outlines.
  The original 48-star layer keeps its layout, sizes and colors. Changes visibly
  relocate only the added layer, with contrasting signature and background colors.
- Thickness rotates the signature 360 degrees and grows it up to 60%. It crosses
  the full sky, wraps to the far left and can pass behind the sun.
- Motion sets the sun's white-to-red-orange pulse depth; Speed sets its rate.
  Sun rise is 25% shorter with the same lowest point. Seven designs remain.
- Blue and green readouts use the software reference palette; green added stars
  share the readout green. Character's pointer makes one 270-degree sweep.
- Thickness, Interval, Motion and Speed have no direct wave-geometry effect.
- Sound generation, saved parameter mappings, stock encoder scrolling and the
  15-preset bank are unchanged. Existing installations need only the plugin.
- Native build, package/relocation/import checks and sanitizer tests pass.
  Final 1.2 installation and combined device performance remain to be checked.
- Existing GIFs are retained for a separate manual media update.

# 1.1.15 — spreading impacts, freer side joints and readout backing

- Replace fixed spring oscillation with a sharp outward-moving pulse, quadratic
  fade and an eighth-strength trailing wake. Keep chord grouping and held-note settling.
- Render waves below the former y=126 cutoff. Draw the black backing only when
  Shift shows the readout; preserve default-off and latched visibility.
- Increase lower side-joint target response from 25% to 100%; free bottom travel
  and constrain neighboring triangle areas to preserve visual continuity.
- Retain the 14 Character styles, star layers, sun, audio code/data and all presets.

# 1.1.14 — selected Character waves, Shift readout and sharper trampoline

- Remove Swell, Cross, Breath, Tilt, Twist and Saddle from the visual choices.
- Map the retained 14 styles in order to the existing 14 Character indices.
- Restore Blue's Morph/Character page toggle; retain audio and saved mappings.
- Shift toggles the complete bottom readout/dial, hidden by default, latched in RAM.
- Stronger, tighter, earlier trampoline hits with a faster rebound and settling.

# 1.1.13 — Morph wave audition and trampoline notes

- Blue press cycles 20 numbered visual wave modes; Blue turn remains Morph.
- Mode 01 retains the original movement; 19 alternatives vary movement.
- Remove Character's wave influence in every mode; retain its audio behavior.
- Temporarily replace Character-page access without changing its saved word.
- Replace continuous audio-shaped diagonal bending with local note/chord impacts.
- Group nearby chord publications, overlap damped rebounds, and settle held notes.
- Keep original/added stars, sun, 60% star growth, audio and presets unchanged.

# 1.1.12 — moving wave joints, visible right tips and larger signature

- Increase lower shoulder response to the shared wave swell from 4% to 25%.
- Bound the bottom joints' downward swing to keep them above the readout.
- Shift all rightmost outline vertices 8 pixels left; joined edges follow.
- Increase maximum signature growth from 30% to 60%; retain one full rotation.
- Widen signature travel margins by 2px per side to fit the larger outline.
- Keep central geometry, small stars, sun, sound, controls and presets intact.

# 1.1.11 — continuous star path, opposite colors and growth

- Signature stars cross the whole sky left to right and wrap to the far left.
  Remove collision placement/search; clip behind Solid and draw behind line suns.
- Blue signatures accompany green added stars; green signatures accompany cool.
- Thickness adds 0–30% size growth alongside its existing full rotation.
- Preserve original and added small-star fields, sound, presets, and wave behavior.

# 1.1.10 — Interval signature stars and Thickness rotation

- One distinct larger outline star per Interval, drifting with the added layer.
- Immediate silhouette and position changes; preserve the original 48 stars.
- Thickness rotates the signature through 360 degrees over its full range.
- Freeze direct Thickness wave response at the prior default; Interval also
  remains independent of wave geometry. Sound and saved controls are unchanged.
- Retain reduced small-star overlays, guide text colors and shorter sun rise.

# 1.1.9 — reduced added stars, lower sun rise and readout colors

- Blue text uses #698eff; Green text and added stars use #00ed95.
  Preserve waves, original stars and existing dial colors.
- Bundle half-count, half-size added stars with the preserved original base.
- Reduce upward sun travel by 25% before whole-pixel rounding; lowest center
  stays at y=56, highest becomes y=36 instead of y=30. Applies to all seven suns.
- Preserve Brightness opening, red-orange pulse, Interval position jumps, cool/green pairing,
  Character pointer, stock controls, sound engine and preset bytes.

# 1.1.8 — red-orange and separate Interval layers

- Reduce only the added layer to half its doubled count, width and length:
  stock-scale overlay profiles, with the original base untouched.

- Give each Interval its own added-star positions, including cool/green pairs.
- Keep added strokes clear of the original 48 stars so they cannot recolor or
  hide the base. Preserve original base positions, colors, sizes and drift.
- Share #ff423f between Orange text, dial, and the sun pulse endpoint.
- Use the design kit screen token; previous text was #ff9740 and sun #ffbe6b.
- Preserve the white pulse endpoint, depth/rate, all sun geometry, original base stars,
  Character pointer, DSP, preset bytes, and stock encoder behavior.
- Local trial; assess the deeper color on the physical OLED before accepting it.

# 1.1.7 — layered stars and bounded Character pointer

- Restore the original 48 cool stars as an Interval-independent base.
- Add the ten Interval patterns on top, doubling count, line length and width.
  Green overlays share the readout palette; they never recolor the base.
- Separate stable overlay positions; total 72–144 stars before sun occlusion.
- Character pointer follows full encoder travel on one 270-degree arc. Names,
  saved decoding, knob sensitivity, audio and presets remain unchanged.
- Check base invariance at eight phases, exact doubled overlays, and all 32767
  Character pointer positions. Native peak graphics/audio cost remains untested.

# 1.1.6 — Interval star fields

- Ten immediate Interval states: five distributions paired with cool/Green tint.
- Green reuses the Interval readout's palette token (#00e19b), with sky shading.
- Sparse large stars through dense tiny stars; no more than 48 particles.
- Move Interval's direct visual response from ocean geometry to particles.
- Preserve Morph/Thickness/note responses and the 1.1.5 sun color pulse.
- Retain stock knob sensitivity, saved interval encoding, DSP and all presets.
- Host checks cover all ten distributions/colors, ocean independence and audio
  equality. Native display and CPU checks remain pending.

# 1.1.5 — white sun with Orange-controlled pulse

- Motion sets sun color depth from white to the existing orange; Speed sets the
  pulse rate using the same rate mapping as the audio LFO.
- Apply the color to all seven designs, including the retained native Solid fill.
- Keep ambient ocean motion at the former default depth/rate. Orange no longer
  changes ocean geometry or travel speed. Morph, Thickness, Interval and note
  responses remain intact.
- Preserve DSP, tables, preset bytes, sun geometry and Brightness behavior.
- Host checks cover color endpoints/depth, all designs, rate and ocean independence.
  Hardware rendering and CPU qualification remain pending.

# 1.1.4 — approved Solid and Lines suns

- Default 01 Solid preserves the approved solid Eclipse from trial 10.
- 02 Lines preserves trial 21: seven direct 4px horizontal strokes with 2px gaps.
- Cycle: 01 Solid → 02 Lines → 03 Halo → 04 Retro → 05 Bands → 06 Saturn → 07 Triple.
- Keep the other approved artwork, wave behavior, sound engine, tails and 15 presets unchanged.
- Replace the Holy Dust showcase with a ten-second idle brightness sweep: no played or held notes.
- Native SVG shapes use explicit paint; host snapshots remain approximations of the hardware raster.

# 1.1.0 — sun rendering refinement

- Mark the approved sound and 15 presets as the 1.0 baseline; no sonic or preset changes.
- Eclipse radius 15→20, fixed-size covering disk, fractional span endpoints and exact two-pixel rows. Separate continuous coverage from whole-pixel vertical travel.
- Fix maximum Brightness stopping one vertical step short of fully open; verify full-disk aspect ratio.
- Retain fractional circle vertices and overlap segment ends to close geometric notches without increasing stroke count.
- Extend host motion checks to every sun with fixed coverage, plus circularity/radial checks. Physical raster confirmation remains pending.

# Pending preset rename adjustment — 2026-10-20

- Use XFIN instead of FIN for the former End Titles sound, before installing either pending renamed file.
- Pending replacements are a fifth tide and XFIN. No sound, plugin or visual change.

# Final core preset names — 2026-10-20

- Rename Fifth Tide to a fifth tide; FIN remains the name for End Titles. Both retain identical sound parameters.
- Retire both old filenames from the generated bank; keep 15 unique factory presets. Preserve the R6 retained-sound comparison under the new name.
- No plugin/audio/visual changes. Post-flash device inventory and browser duplicate check remain pending.

# Sun correction and upper mountain notes — 2026-10-20

- Default/cycle: original 05 ECLIPSE, 01 HALO, 02 RETRO, 03 BANDS, 10 SATURN, 18 TRIPLE. Original IDs stay visible; Broken is absent.
- Restore 05's original crescent drawing and scanline spacing; animate only coverage, from full sun to fully covered. Remove the invented dotted rim. Remove Retro's bottom stripe.
- Raise the marked upper mountains on both sides, increase shoulder/inner motion and quiet the corner tips. Lower the readout blackout boundary by 5 px.
- Rename End Titles to FIN without changing its sound. Keep Fifth Tide's name pending the user's decision about an alphabetical prefix.

# Selected suns — 2026-10-20

- Keep original 01 Halo, 02 Retro, 03 Bands, 05 Crescent, 10 Saturn and 18 Triple. Original IDs appear in the readout and preview filenames; no positional renumbering.
- 05 retains horizontal strokes and transitions from full high sun through a crescent to an eclipsed striped rim as it descends.
- Enlarged 18 remains the default; enlarged 02 and integer endpoint placement remain.
- R6 wave/mountain geometry and water boundary remain restored. Approved 15-sound core and audio processing are untouched.

# Wave rollback — 2026-10-20

- Reverted the latest wave geometry changes: top side peak 32→35, shoulder x 53→43 and y 56→59; water clipping boundary 126→121.
- Preserved the approved 15-preset core, sound/Character assignments, dial alignment, particle slowing and current nine suns.
- Sun selection is pending a new choice by printed name from the original 20-design picture sheet; no additional sun redesign or renumbering in this build.

# Core set selection — 2026-10-20

- User approved 15 sounds as the core set: eight older sounds including Fifth Tide, plus Holy Dust, End Titles, Ice Chapel, Last Light, Moon Choir, Stone Hall and Tidal Arc.
- Retired the other 13 R7 evaluation presets from the active bank. Archived deliveries remain unchanged.
- Locked the core preset list and existing 14 Character assignments in data/core_set.json; generation rejects accidental renumbering.
- No production plugin, DSP, waveform, visual or envelope changes. Corrected R7 keeper files remain authoritative; R6 Character values must not be reused.
- Device inspection found one exported file per name, with the seven older keepers still carrying R6 Character words. Browser doubles were reported but not directly enumerable on the USB disk; removal/sync/reimport must be verified on hardware.

# Revision record

## 0.7.0-r7 — cinematic pads and focused sun set

- Keep seven selected R6 sounds, with exact dry output equivalence; Deep Pressure saves T3 Reverb enabled. Remove other factory presets, add 20 cinematic pads (27 total).
- Audit confirms two playback heads since R1. Keep production DSP, six voices and release processing unchanged. New long-loop seven-cloud PADsynth sources; original WARM preserved.
- Compact Character choices to 14. Matching R7 presets migrate the seven retained Character words; R6 custom presets require remapping.
- Suns 18/1/2/3/5/7/10/12/16 become 1–9. Enlarge triple/retro, snap endpoints, animate eclipse coverage with height.
- Extend and raise top mountain shoulders; raise dial 3 px, lower water boundary 5 px. Half the former daytime particle speed; preserve night speed.
- Host/native validation and retained-sound/CPU comparisons; device timing and native raster check remain pending. No device writes.

## 0.6.0-r6 — distinct sound sources, text encoding fix and secret screen

- User reports R5's 40 presets are too similar/string-like. Preserve only Fifth
  Tide; replace 39 presets with 39 unique source spectra and varied envelopes,
  intervals, register and motion. Forty named Character choices, 41 endpoints.
- Keep Fifth Tide's AIF and sixteen WARM table arrays unchanged. Compact banks
  share the existing pitch increment, with one extra table-index mask. No new
  runtime oscillators, FFTs, voice reduction or shortened release processing.
- Correct stock font encoding: lowercase ASCII selects letters; uppercase
  selected symbols. Keep the approved dial, numeric formatting and text size.
- Raise the top side peaks 5 px and extend their shoulders 9 px inward / 4 px up.
- User approved blue → green → orange → white, then 7 seconds idle, in place of
  an unsupported continuous button hold. Three small bouncing native labels;
  movement/press cancels or exits. No core hook or companion plugin.
- Full host/native checks and Fifth Tide equivalence recorded; hardware sound,
  font, input and CPU retest pending. White press/redraw limitation remains.


## 0.5.0-r5 — mountain facets, sun selection and active readout

- Preserve the approved R4 key response, DSP, tables, six voices, release tails
  and all 40 preset files. All 44 reference audio fixtures are byte-identical.
- Restore pointed, quiet side facets using the user's green/red marked photo;
  check orientation through 500 animated control/phase combinations.
- Twenty primitive sun designs; white page toggle used as a one-shot on redraw.
  Core limitation: rapid press+turn can reach shared Speed before acknowledgement;
  wait for the sun change. No core change or independent callback is claimed.
- 48 particles: 24 small / 16 medium / 8 large, with daytime slowing/dimming
  and nighttime wandering/brightening.
- One active-control dial and native 12 px stock-font row. Exact synth-menu
  size and physical button timing still need a device check.
- WARM/VOICE cyclic Character selection in 256-unit steps; factory anchors
  retained, other custom saved Character words may need reselection.
- Native compile/package and sanitizer checks pass. R5 hardware retest pending.


## 0.4.0-r4 — visual refinement and 40-preset evaluation bank

- User reports R3 CPU seems good and approves line weights. Keep optimized
  DSP loop, PADsynth tables, six voices, line widths and Fifth Tide release.
- Preserve Fifth Tide byte-for-byte; replace the other three factory sounds
  with 39 new dry pads (40 total), catalog and one dry WAV per preset.
- Ten equal-tempered interval options, narrower selector bins; legacy
  8192/16384/30000 retain unison/fifth/octave. Other saved values may remap.
- 66-vertex/165-edge mesh; ordered side rows and mirrored edge diagonals.
- Visual speed capped near the former 0.54 Hz appearance, independent of audio.
- 50% larger sun with wider travel and unchanged stroke.
- Continuous within-octave pitch mapping, normalized visual trace and a bounded
  note-on response. Notes bend joining lines, never the wave vertices.
- Single-note visual regression, 500 side-orientation checks, 40 preset
  round trips and R3 audio equivalence checks. Hardware R4 retest pending.

## 0.3.0-r3 — playback CPU reduction and visual corrections

- R2 feedback: Fifth Tide preferred; approximately 53% AUDIO peak for one
  note, 80% for three; overlapping release tails crackle/skip. Conditions
  incomplete. No R3 hardware measurements yet.
- Preserve R2 tables, all four preset files, six-voice capability and T2 tails.
- Single guarded playback level, one bounded bank fade, block-prepared
  coefficient ramps, constant-pitch fast path, inlined reads, symmetric FIR,
  staggered telemetry. No FFT or new allocation on device.
- More active baseline waves; smaller triangles; thicker strokes; spiky
  outer vertices at about 4% response. Notes bend joining lines only.
- Remove fixed horizon and rectangular sun exclusion/particle relocation.
- Added silence regression check and fixed-phase visual reactivity test.
  Native build/package and sanitizer checks pass; actual crackle resolution
  and subjective/native appearance remain for hardware retest.

## 0.2.0-r2 — pad and visual redesign after first device feedback

- R1 device feedback: intermittent crackling on single notes or chords across
  presets; tone described as brassy and insufficiently lush. Cause unconfirmed.
- Baked seven detuned Gaussian clouds per harmonic, softer spectral shaping,
  broader vowel shelves and 256-cycle loops (four times longer at a fixed note).
- Half-rate table/filter engine with eight-tap output reconstruction; shared
  phase decoding and block-prepared table pointers; six-note polyphony retained.
- Revised four presets and defaults; same plugin ID and seven-control schema.
- Always-moving gentle gradient-noise surface, stronger audio/control waves,
  wider 40-vertex/95-edge mesh, larger sun and thicker lines.
- Removed coastal scenery. Forty sky particles, four larger; no horizon-only
  particle band. Replaced 64-bit clipping divisions with bounded int32 clipping.
- Eight dry host WAVs and three host animations supplied. R2 subsequently tested as recorded above; no claim that CPU overload or crackling has been resolved.

## Working-name update — VAPORA

- Renamed the plugin, source files, build outputs and preset type to `vapora`.
- Preserved DSP behavior, table data, controls and visuals.
- Added local listening and preview evaluation instructions.
- Previous PADSEA bundles remain historical candidates with a distinct ID;
  existing `padsea` presets are not automatically migrated.

## 0.1.0-r1 — first hardware-test candidate

Sound:
- Reproducible WARM felt/silk and VOICE ooh/aah PADsynth masters and eight
  coherent filtered levels per endpoint.
- Two heads, seven stable saved controls, parameter smoothing, family fades,
  zero-gain Interval changes, two-pole tone shaping, normal velocity/T2.
- Conservative mono output gain; four native-format pad examples.

Visual:
- Original blue wireframe ocean, small rising sun, coastal contours and lighthouse.
- 54 vertices / 133 unique edges, clipped water, independent colored readouts.
- Envelope-weighted trace through the whole mesh, pitch spacing, sparse points
  and an energy-triggered glint.

Evidence:
- Untouched template and real candidate compiled with Blackfin GCC 13.3.0.
- Host sanitizers, deterministic assets, actual package relocation/descriptor
  checks, WAV fixtures and host previews.
- No native emulator or physical OP-1 test; no known-good hardware revision yet.

Development history is retained in `build/vapora-history/` outside the source
folder. Do not treat an earlier development artifact as hardware-approved.
