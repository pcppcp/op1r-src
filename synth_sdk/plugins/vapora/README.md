# VAPORA 1.2.3

Six-voice PADsynth instrument for the original OP-1. This contribution adapts
the published Hundekraft 3.0.2 release to op1r 45 (`c378c29`); the `.op1p`
build and host tests pass. Device testing below was on Hundekraft, not op1r.

The visual-first interface is inspired by TE Voltage and the stock instruments.
The scene represents the controls; Shift reveals numerical/named readouts.
See [TESTING.md](TESTING.md) for measured results and remaining checks.
Two independently phased, detuned playback heads per voice; 14 sound Characters
and 15 factory presets. The OP-1 supplies velocity, T2 envelope and optional T3
processing. This release raises fixed output gain by 3x (about +9.54 dB), retaining
table data, sound-word mappings and visuals. For this op1r contribution,
Stone Hall saves T3 off and retains its sound settings and octave -3;
the other fourteen preset files are unchanged. A note-off callback now
tracks held serials solely for the screen; it does not alter DSP or T2 release.

## Controls and visuals

| Encoder | Turn | Press, then turn |
|---|---|---|
| Blue | Morph: sound blend and wave movement | Character: sound family and wave style |
| Green | Thickness: detune/fullness; signature rotation and size | Interval: second-head pitch and added star field |
| White | Brightness: filter and sun height/opening | Each press cycles the seven suns |
| Orange | Motion: sound modulation and sun pulse depth | Speed: modulation and sun pulse rate |

Blue, green and orange presses toggle their main/secondary control. White turns
always return to Brightness after the next redraw. White and orange share
secondary word 7 in this core: wait for the sun change before turning white.
A turn before redraw can reach Speed; clicks within one redraw can coalesce.
The firmware's stock knob scrolling and saved-word encoding are retained.

Shift toggles the complete bottom control readout and dial, initially hidden.
It stays in the chosen state across screens and slots during the session;
visibility is not saved in sounds or across a fresh plugin load. Knob edits do
not reveal a hidden readout. Its black backing appears only when shown. The
underlying waves always use the full available area, clipped at y=151 instead
of the former y=126 boundary. Hiding the readout exposes those same waves.

Shift reads the R246 core's read-only state byte (also used by op1r). No firmware
patch or companion is required. A Shift tap entirely between redraws can be
missed; a Shift already held on screen re-entry does not toggle the readout.

## Character waves

Character selects the following visual styles in existing audio Character order.
The readout names the sound Character (Warm, Swarm, Breath, etc.), not its
wave style. Morph changes wave movement. The pointer follows one 270-degree sweep over the
raw encoder word while the names retain the firmware's usual cycling.

| Character | Wave style | Existing sound |
|---|---|---|
| 01 | Original | Warm |
| 02 | Ripples | Swarm |
| 03 | Roll | Breath |
| 04 | Fold | Flute |
| 05 | Orbit | Pressure |
| 06 | Rings | Surf |
| 07 | Rain | Porcelain |
| 08 | Pulse | Sub |
| 09 | Tide | Velvet |
| 10 | Ribbons | Cathedral |
| 11 | Weave | Nebula |
| 12 | Dunes | Aurora |
| 13 | Echo | Oceanic |
| 14 | Jelly | Glassveil |


Each key or grouped chord creates a sharp hit at a random interior location.
One ripple moves outward about 4.5 grid cells as its amplitude falls to zero;
a small trailing wake is one eighth of the main pulse's strength. It settles in
about 1.4 seconds after a tap. Holding a participating note slows the fade
slightly and lets the same front travel farther, capped at about 1.75 seconds.
Holding also broadens the trailing return up to twice its width and softens
the rebound, while preserving the sharp incoming edge and the 1.75s cap.
Release stops earning extension; an unrelated held note or reused voice cannot
sustain an older hit. Even an indefinitely held chord settles completely. Held
notes never produce continuous audio-trace bending. Overlapping hits share an
8px bound, with local-spacing limits and absorption near the rim.

Lower side joints target the full wave displacement, up from 25%, subject to
neighboring triangle limits. Bottom joints can travel into the former readout
strip. The right outline is inset 8px to keep the pointed corners visible.
Thickness, Interval, Motion and Speed have no direct wave-geometry effect.

## Interval stars and Thickness

The original 48 cool stars (8 large, 16 medium, 24 small) retain their layout,
sizes, colors and drift. Interval adds a distinct seeded field on top; changing
Interval immediately replaces its placement. Returning to an Interval restores
its arrangement at the current animation phase. Added strokes avoid the base.

| Interval | Added distribution (before sun occlusion), stock size | Color |
|---|---|---|
| Octave below | 8 large, 4 medium | Cool |
| Fifth below | 8 large, 4 medium | Green |
| Unison | 8 large, 8 medium, 16 small | Cool |
| Minor third | 8 large, 8 medium, 16 small | Green |
| Major third | 18 medium, 6 small | Cool |
| Fifth | 18 medium, 6 small | Green |
| Major sixth | 8 medium, 32 small | Cool |
| Minor seventh | 8 medium, 32 small | Green |
| Major seventh | 48 tiny | Cool |
| Octave above | 48 tiny | Green |


Each Interval also adds a unique outline star, 14–18px at minimum Thickness:
paired four-, five-, six-, seven- and eight-point silhouettes with differing
proportions and rotations. Blue signatures appear over green added stars, green
signatures over cool stars. The signature drifts the full left-to-right path,
wraps to the far left and passes behind the sun. Original stars draw over it.
Thickness rotates once through 360 degrees and grows the outline from 100% to
160%; its line width remains 1.5px. Audio behavior is unchanged.

## Sun

01 Solid (default) → 02 Lines → 03 Halo → 04 Retro → 05 Bands → 06 Saturn →
07 Triple, then Solid. Solid uses the approved native cubic disk/cover; Lines
uses seven horizontal strokes with 4px widths and 2px gaps. Other approved
shapes remain, including the removed lowest Retro stripe and enlarged Triple.

Brightness raises the center from y=56 to y=36, a 25% shorter rise than before,
and controls opening. Motion zero keeps the sun white. Increasing Motion sets
pulse depth toward red-orange #ff423f; Speed sets about 0.02–2 Hz. This visual
LFO is free-running, not a phase-accurate meter of individual audio voices.
The DSP still applies Motion to morph, detune and brightness. The ocean uses its
former default ambient Motion/Speed independently of the orange controls.

Blue readouts use #698eff; green readouts and added green stars use #00ed95.
Orange readout, dial and pulse endpoint share #ff423f. These are reference
software colors from the OP-1 Design Kit, not calibrated OLED measurements.
Wave colors, base stars and the other dial paints remain unchanged.

## Presets and installation

Build `vapora.op1p` with the SDK packer and install it using op1r's plugin
workflow (`synth_sdk/PLUGINS.md`). Copy the optional AIF bank into `synth/vapora`
in normal disk mode. The existing Hundekraft `.knl` is a separate release format.
Deep Pressure requires Reverb. Stone Hall is saved dry so it has no Plate
dependency; the other thirteen recipes are also dry. Native preset import,
FX mapping and save/reload on op1r remain to be verified.

Do not replace current presets with older R6 evaluation files: their Character
words select different sources. `data/core_set.json` locks the final names and
Character assignments; `PRESET_CATALOG.csv` and `presets/recipes.json` document
this bank. Save edits using the OP-1's usual sound-key gesture.

Deep Pressure saves the separate Hundekraft Reverb effect (time 70%, tone 46%,
diffusion 73%, mix 31%) and requires that effect to be installed. Stone Hall
starts at octave -3 with T3 off. Fourteen presets are dry. Host fixtures do not include
native T2 or T3 effects.

## Sound and implementation

The two-head, six-voice DSP has a fixed 3x output boost relative to 1.2.2.
There is no compressor or automatic level adjustment. Offline PADsynth tables contain
broadened spectral components; these are not extra runtime oscillators. There
are 20 endpoints and 448,800 sample bytes, including the original two long Warm
tables and six 8,192-sample cinematic spectra. Half-rate synthesis, block-rate
preparation, guarded table transitions and staggered telemetry remain as
recorded in `CPU_AUDIT.md`. No device CPU percentage is inferred from host tests.

Animation state is not stored in hidden sound parameters. The existing secret
remains: from normal pages, press blue → green → orange → white, then wait seven
seconds without changes. The three bouncing labels exit on a knob action.

## Rebuild and validation

From `synth_sdk`, the standard build is:

```sh
python3 make_plugin.py vapora
python3 harness/audio.py vapora
```

The standard packer uses the SDK Docker toolchain. For the complete VAPORA
host suite plus a native Blackfin toolchain build:

```sh
python3 -m pip install -r plugins/vapora/requirements.txt
python3 plugins/vapora/tools/build.py --toolchain /path/to/toolchain --skip-previews
```

The toolchain directory contains `bfin-gcc/bin` and `bfin/bin`. Omit
`--toolchain` to use Docker. Checked-in tables allow the standard packer to
build without NumPy/Pillow or regeneration. `preview.png` is a host approximation
with a substitute font; `audio_vapora.wav` comes from the upstream audio harness.
The build scripts do not install anything on the device.

Additional dry headroom sweep (13,440 six-voice cases):

```sh
clang -O2 -std=c11 -I . tests/output_level.c vapora_dsp.c -lm -o /tmp/vapora-level-check
/tmp/vapora-level-check
```

The sweep tests actual independently phased voices, including repeated-note
unisons. It does not guarantee arbitrary phase-aligned sums or downstream FX
headroom. Compare on hardware before publication.

See `HARDWARE_CHECKLIST.md`, `CPU_AUDIT.md`, `TOOLCHAIN.md`, `CHANGELOG.md` and
retained license/third-party notices. PADsynth algorithm by Nasca Octavian Paul.
