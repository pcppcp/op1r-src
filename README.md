# op1r-src

Source code of **op1r**, custom firmware for the Teenage Engineering OP-1 (firmware R246): custom synth engines and
effects with their own screens, quad LFO, euclid and dots sequencers, USB audio track outs, tape markers, count-in,
punch in/out, sample editor, resource monitor and more.

- **Users:** you don't need this repository. Patch your firmware with
  [op1r](https://github.com/pcppcp/op1r) (also in the browser).
- **Plugin authors** (new synths / effects): read [synth_sdk/PLUGINS.md](synth_sdk/PLUGINS.md). You can write and
  test a plugin on a PC without any firmware.
- **Firmware builders:** see below.

This repository contains no Teenage Engineering firmware. Building needs your own copy of the official
`op1_246.op1`. The next op1r release (op1r 27) is the first one built from this repository.

## Layout

| path | content |
|---|---|
| `synth_sdk/plugins/` | synth engines and effects, one folder each (`plugin.json` + C + screen) |
| `synth_sdk/template/` | starting points for a new synth / effect |
| `synth_sdk/op1_plugin.h` | the plugin API |
| `synth_sdk/dispatcher.c`, `hook.S`, `link.ld` | hooks into the firmware |
| `synth_sdk/screens/`, `seq/`, `lfo/`, `tape/` | core features (screens, sequencers, quad LFO, tape tools) |
| `synth_sdk/display/` | SVG generators for the screens |
| `synth_sdk/harness/` | host tests and renderers (screens to PNG, plugins to WAV) |
| `synth_sdk/make_firmware.py` | builds the firmware |
| `tools/` | MIDI / USB test scripts |
| `toolchain/Dockerfile` | Blackfin cross compiler (bfin-elf gcc 12) |

## Testing on a PC (no firmware needed)

Requirements: a C compiler, Python 3 with pycairo (screen previews) and numpy (some tests).

```sh
cd synth_sdk
python3 harness/audio.py supersaw           # plugin -> harness/out/audio_supersaw.wav
python3 harness/render.py tape out.png      # plugin screen preview
python3 harness/render_euclid.py            # other screens: harness/render_*.py
```

## Building the firmware

Requirements: Docker, Python 3, the original `op1_246.op1`, and
[op1repacker](https://github.com/op1hacks/op1repacker).

```sh
docker build -t bfin-elf-toolchain toolchain                      # once, takes a while
git clone https://github.com/op1hacks/op1repacker.git
python3 -m venv op1repacker/.venv && op1repacker/.venv/bin/pip install -e op1repacker
cp /path/to/op1_246.op1 . && op1repacker/.venv/bin/op1repacker unpack op1_246.op1    # -> op1_246/
cd synth_sdk && python3 make_firmware.py --opz                    # -> ../op1_246_custom-repacked.op1
```

The boot screen of a local build shows `op1r <n>` and the build time. Install it like any OP-1 update (bootloader
update mode); the bootloader is not modified, so another image can always be flashed (see below before going back
to stock or to an older op1r). **Use at your own risk**: a broken build can lock the OP-1 until you flash a working
image.

Some code runs from the firmware's encrypted part. Those few patches are precomputed in `synth_sdk/l1_patches.json`
and target a fixed jump table at the start of the code (`hook.S`, `.l1vec`). So plugins and most other changes
build without them changing. Changing the L1 patch lists themselves needs a maintainer.

## Flashing back and downgrading

op1r keeps its settings in the OP-1's own user data, and some of them stop older firmware at boot (an assert on
every boot). The OP-1 is not bricked: flash the current op1r again from bootloader update mode, change the data, then
flash the older image.

**Back to stock R246.** Stock R246 asserts at boot on what it doesn't know. First, on op1r:
- give every synth sound a stock engine, stock FX and a stock LFO (no plugin engine or FX, no quad or pond LFO), and
  the drum sounds stock FX;
- give every sound a stock sequencer (not euclid, dots, chord, grids, oracle, peal or another plugin);
- keep at most 42 sampler presets;
- don't leave a drum kit saved by drum+ 2.6 – 2.7.1 in a drum slot (R246 rejects its gate play mode).

**Older op1r.** Don't go below op1r 54, and below 64 only with every quad LFO unmuted:
- below 64, a sound with a muted quad LFO (Shift + push on the LFO page) stops the boot (error 27): the mute is
  stored in the LFO's marker, which older releases don't accept. Unmute them first;
- below 54, more than 42 sampler presets assert at every boot (54 skips the extra rows);
- below 53, pond LFO sounds load as tremolo (off) and lose their settings when saved; the Lorenz quad LFO waves
  load as S&H;
- below 51, quad LFO targets on a page-2 knob fall back to another target (saved so once the LFO is edited);
- 45 and older cut the plugin list at the sampler+ entry and rewrite it (later plugins get new numbers);
- below 36 an unknown LFO type, below 35 an unknown sequencer, stops the boot.

From 54 on nothing locks or is lost, but sounds can change until you upgrade again: below 57 many plugin engines
run their times at half their value and their filters an octave higher (the 88.2 kHz voice rate came in 57), below
58 the master LFO is off, below 60 granular ignores pitch and shape, below 61 long "in a room" free loops are shorter,
and plugins newer than the release don't load.

## License

GPL-3.0-or-later with an additional permission to combine it with the OP-1 firmware: see [LICENSE](LICENSE) and
[NOTICE](NOTICE), which also lists the third-party parts (MIT) and what is not covered. Contributions (e.g.
plugins) are accepted under the same terms.
