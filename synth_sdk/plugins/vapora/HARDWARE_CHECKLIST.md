# op1r validation remaining

The `.op1p` builds against op1r 45 but has not been installed/tested on that
firmware. Verify loading, Shift, all controls, preset import/save/reload, missing
FX handling, and playing with tape plus FX. Read [TESTING.md](TESTING.md) for
completed Hundekraft measurements. The release checklist below is retained
for reference. Stone Hall is dry in this contribution.

# VAPORA 1.2.3 hardware checks

Output is approximately 9.54 dB louder. Stone Hall now saves T3 off;
the other 14 preset files remain unchanged. Update the plugin and replace only
Stone Hall's AIF. Start with the master volume lower for this comparison.

- Load Stone Hall and verify T3 is off and octave is -3. Confirm these survive reload.

- Compare single notes and six-note chords with 1.2.2 and other synths at the
  same master setting. Listen for clean peaks, comfortable level and dynamics.
- Check all Characters at high Brightness/Thickness, including low notes,
  dense clusters, retriggers and long releases. Host sweeps are not a proof
  for every possible note/control/phase combination on the device.
- Check saved FX (especially Deep Pressure Reverb), master Drive and tape
  recording levels: the stronger signal also feeds those downstream stages.
- Keep the 1.2.2 plugin available for a direct comparison or rollback.

- Push Blue to toggle Morph/Character; verify the 14 retained styles in order.
- Turn Character: the wave style and existing audio Character change together.
  The readout must show the sound family (Warm, Swarm, Breath, etc.), not the wave style.
  All preset Character words and audio sources remain unchanged.
- Verify the bottom dial/readout starts hidden on fresh load. Shift toggles it
  on persistently, release leaves it on, and the next Shift press hides it.
- Holding Shift must not repeat-toggle. Knob turns must not reveal a hidden footer.
- The visibility choice should survive screen/slot changes during the session.
- Hidden readout: waves must continue below the old cutoff. Its black backing
  must appear with the readout and disappear on the next Shift press.
- A key or chord should make a sharp hit that spreads outward and fades in about
  1.4 seconds, with a small wake rather than repeated elastic bouncing.
- Compare a quick tap, a half-second hold and a sustained chord. The held hit
  should propagate slightly farther and linger longer, never beyond about 1.75s.
- Compare with 1.2.1: held ripples should ease back more slowly with a softer
  rebound, while quick taps and the final time cap remain unchanged.
- Release some chord notes, then the rest; only notes belonging to that impact
  should extend it. A reused voice or unrelated held note must not sustain it.
- Fast playing should overlap impacts without folded side triangles.
- Held audio should stay settled; returning to the screen should not replay notes.

- Compare with IMG_0112: the green-marked lower joints should sway with the
  rest of the wave much farther, without folding adjacent triangles.
  Bottom joints can travel behind the visible readout and are exposed when hidden.
- The maroon-marked right tips are now 8 pixels farther left. Verify the whole
  pointed outline stays visible and its joined edges remain continuous.

- Blue font is #698eff; Green font and added stars are #00ed95.
  Waves, original stars and dial colors retain their existing palette.

- Confirm 01 Solid is the default after restart.
- Confirm the cycle: 01 Solid, 02 Lines, 03 Halo, 04 Retro, 05 Bands,
  06 Saturn, 07 Triple, then back to Solid.
- The user approved Solid (trial 10) and Lines (trial 21) on hardware;
  this combined build preserves those methods and still needs an installation check.
- Sweep Brightness on both; Lines has seven 4px strokes with 2px gaps.
- Sun rise is 25% shorter (whole-pixel rounding): center y=56 to y=36.
  Lowest position and opening behavior are unchanged; check all seven designs.
- At Motion zero, confirm each sun is white throughout the Speed range.
- Increase Motion: the sun pulses toward orange, reaching the shared red-orange (#ff423f)
  at full depth. Speed changes the pulse period (about 50 seconds to 0.5 seconds).
- Check all seven designs, including the native Solid fill, for consistent tint.
- Compare Motion/Speed text and dial against the fully tinted sun: same red-orange.
- With no notes, Orange and Interval must not change ocean shape or travel speed.
  Morph controls the selected visual mode; notes make temporary trampoline impacts. Thickness and Interval
  must have no direct wave response.
- Step through all ten Intervals: particle sizes/counts and cool/Green tint snap
  immediately in the added overlay. Every step also relocates that layer.
  The original 48 cool stars remain unchanged and are never overpainted.
  Green should match the readout hue; stock knob speed is retained.
- Each Interval has a distinct signature: blue over green stars, green over cool.
- Check a full left-to-right pass and far-left wrap, with no collision jumps.
- Verify the signature passes behind Solid and the other six sun designs.
- Thickness rotates one full turn and grows the signature up to 60%; waves stay fixed.
- Check reduced overlay sizes/counts and unchanged original stars on the OLED, especially occlusion at the sun.
- Character's pointer makes one 270-degree sweep; page presses do not edit sound words.
- Check peak graphics/audio load with the densest overlay, six voices and T3 FX.
- Dial alignment, Brightness height/opening and slower daytime particles remain.
- Preserve the other 14 installed sounds; replace only Stone Hall's preset.
- Confirm ordinary playback and screen switching. Tables and screen code are
  unchanged; DSP has the fixed output gain increase. Host checks do not
  establish hardware CPU timing.

No format or reset is part of this update. Reload Stone Hall after replacement.
