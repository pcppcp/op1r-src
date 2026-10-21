# op1r-src

Source code of **op1r**, custom firmware for the Teenage Engineering OP-1 (firmware R246): custom synth engines and
effects with their own screens, quad LFO, euclid and dots sequencers, USB audio track outs, tape markers, count-in,
punch in/out, sample editor, resource monitor and more.

Physical keys use per-key debounce: presses respond immediately, and releases settle to suppress double triggers.
On the clock/card screen, press **T3** and turn **green** to select **OFF** or **1–30 ms** (default **5 ms**);
the setting is saved across restarts. MIDI and sequencer events bypass the filter. Implementation, tests and hardware checks:
[key debounce](synth_sdk/input/README.md).

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
update mode); the original firmware can always be flashed back. **Use at your own risk**: a broken build can lock
the OP-1 until you flash a working image.

Some code runs from the firmware's encrypted part. Those few patches are precomputed in `synth_sdk/l1_patches.json`
and target a fixed jump table at the start of the code (`hook.S`, `.l1vec`). So plugins and most other changes
build without them changing. Changing the L1 patch lists themselves needs a maintainer.

## License

GPL-3.0-or-later with an additional permission to combine it with the OP-1 firmware: see [LICENSE](LICENSE) and
[NOTICE](NOTICE), which also lists the third-party parts (MIT) and what is not covered. Contributions (e.g.
plugins) are accepted under the same terms.
