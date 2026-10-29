# Writing op1r plugins (synth engines and effects)

A plugin is one folder with a `plugin.json` and C code. It becomes a new synth engine or FX in the OP-1's lists.
You can write and test it on a PC; the op1r maintainers build it into a release. License: like all of op1r,
GPL-3.0-or-later with the OP-1 firmware permission (see `LICENSE` and `NOTICE` in the repository root).

## Quick start

```sh
cp -r template/synth plugins/mysynth          # or template/fx
# rename mysynth.c, the symbol (ENGINE_mysynth / FX_myfx) and "name" in plugin.json
python3 harness/audio.py mysynth              # -> harness/out/audio_mysynth.wav: listen to it
python3 harness/audio.py mysynth --knobs 0,32767,8000,20000
```

You need a C compiler and Python 3 (with numpy only for some of the other tests). `audio.py` builds your plugin
with address and undefined-behaviour sanitizers: out-of-bounds writes and integer overflows are reported there,
before they can crash an OP-1.

- **Synths** play a short phrase (velocities 127 and 64 alternating). Engines that use the slot's sampler
  recording get a 2 s synthetic recording.
- **FX** process plucked saw notes followed by 1.5 s of silence, so you can hear tails and feedback.

## plugin.json

```json
{ "name": "mysynth", "kind": "synth", "symbol": "ENGINE_mysynth",
  "sources": ["mysynth.c"], "defaults": [4000, 12000, 6000, 24000] }
```

- `name`: shown on the OP-1; lowercase, at most 15 characters, not a stock type name.
- `kind`: `synth`, `fx` or `tool` (see Tool plugins).
- `order` (optional): leave it out. Sounds find a plugin file by its name. Only plugins that were compiled into
  earlier releases keep their old number here.
- `symbol`: your `op1_engine_t` / `op1_fx_t` object; `sources`: C files, the first one defines `symbol`.
- `defaults`: initial positions of the 4 knobs (blue, green, white, red), 0..32767.
- Optional: `page2_defaults` (FX second-page knobs), `uses_sample` (synth reads the slot's sampler recording),
  `source_cflags` (`{"file.c": ["-O3"]}`: extra optimisation flag for one source file, after the default `-O2`),
  `screen` (own screen, see below). All fields are documented in `plugins.py`.

## The API (op1_plugin.h)

Include only `op1_plugin.h`.

**Synth engine** (`op1_engine_t`): `note_on(self, params)`, `render(self, out[128], params)`, `note_off(self)`.
- 6 voices; `self` is the voice object: the first word belongs to the firmware, the next 32 bytes are yours.
- `OP1_PITCH_INC[n]`: phase increment per sample of this note (2^32 = one cycle).
  `op1_voice_vel_gain(self)`: note velocity as a Q15 gain.
- `params[0..3]`: the 4 knobs, 0..32767. The firmware's envelope and level are applied after `render`.
- `OP1_VOICE_ENV`: the stock envelope (T2) of the voice being rendered, `OP1_BLOCK` int16 values 0..32767 (velocity
  and level included), computed just before `render`. Use it inside `render` to modulate the timbre (cutoff, FM depth,
  fold amount, …); the firmware still applies it to the volume afterwards. Only valid during `render`.

**Effect** (`op1_fx_t`): `init(pos)`, `render(pos, buf[64], params)` (in place).
- `pos`: 0 synth FX, 1 drum FX, 2 master FX (mono sum, mixed back as the wet part). Keep all state per position
  (e.g. `static state_t st[3]`): the same FX can run in several positions at once.
- Knobs: convert with `op1_fx_param_q15(params, i)` (raw words -> 0..32767).
- Memory: `op1_fx_scratch(pos)`, 176 KB per position (shared with the stock FX: it starts with foreign data, clear
  or track what you use).
- More memory: static arrays (bss) are allocated from the firmware heap when the plugin loads: up to 2 MB per
  plugin, 4 MB for all plugins together (about 9 MB of the heap is free, docs/02). Taken as long as the plugin is
  installed, used or not.
- Stereo on the master: set `render_stereo(pos, l[64], r[64], params)` in `op1_fx_t` and `"stereo": true` in
  plugin.json; the master position then gets the mix's own left / right instead of the mono sum. Synth / drum FX
  still get `render` (their bus is mono).

**Helpers**: `OP1_SINE_TABLE` (8192 x int16), `op1_sat16`, `op1_mul16` (Q15). Second knob pages and latching
presses: the `press` field (see the comments in `op1_plugin.h`).

**Coarse knobs** (synth engines): `"coarse": [0]` in plugin.json turns knob 0 (both pages) in 8 steps over its
range instead of the stock ~50 (a selector, a mode); firmware op1r 44 and later.

**Debugging**: `op1_dbg("text", a, b)` writes `dbg: text a b` to the op1emu log (`tools/emu_ctl.py dbg`), stderr in
host builds, and does nothing on the OP-1 (an empty call: fine in any thread, but not per sample).

## Rules (the code runs in the audio interrupt)

A crash, a very long block or an endless loop locks the OP-1 until it is switched off.

- **Integers only.** There is no FPU; float is emulated and far too slow. Use Q15 fixed point.
- **No division or modulo per sample**, no 64-bit arithmetic per sample (both are slow library calls). Per block
  (once per `render`) is fine. Multiply by a precomputed reciprocal instead.
- **No libc**: no malloc, printf, memcpy, memset. Write the loops.
- Keep the per-sample work small: the OP-1 has one DSP core shared by the instrument, FX, tape and UI.
  The resource monitor (clock screen + T1 on the OP-1) shows the load.
- Saturate before storing 16-bit values (`op1_sat16`).

## Own screen (optional)

Without `screen`, synths use the stock iter graphics and FX a generic page with the 4 knobs. For an own screen:
- an SVG (320 x 160, OP-1 line style; the file name must not clash with the OP-1's own `content/display` files),
- `op1_screen_t` with `init(scr)` (look up SVG nodes by id) and `update(scr, dt)` (every frame: move nodes, draw
  lines),
- `"screen": { "svg": "mysynth.svg", "symbol": "SCREEN_mysynth" }` in `plugin.json`.

Preview: `python3 harness/render.py mysynth out.png`. Look at `plugins/supersaw` (synth) or `plugins/tape` (FX).
Colours: blue #4D9EFF, green #00ED95, white #FFFFFF, red #FF3A5D, grey #6F7173. Line ends are flat and zero-length
lines are not drawn (draw a dot as two short crossing lines).

## Tool plugins

`"kind": "tool"`, `symbol` an `op1_tool_t` (op1_plugin.h). A tool gets the keys after op1r's own (update prompt,
MIDI sequencer, drum+, sample editor) and before the stock handlers (`key`, KeyHandler thread, 1 = swallowed, its
releases too), a call every main-loop pass (`service`) and, after `op1_overlay(&tool)`, its `draw(raster)` (GUI
thread) over the current page each frame until it returns 0. `op1_raster_dim(r)` darkens the page first, as HELP
does. Tools are always plugin files; example: `plugins/randomiser`.

## Plugin file (no firmware build)

```sh
python3 make_plugin.py plugins/mysynth        # or a plugin name -> build/plugins/mysynth.op1p
python3 ../tools/op1push.py <tree> --port <midi port> --files plugins/mysynth.op1p   # tree/plugins/ holds the file
```

The OP-1 (op1r with the plugin loader) loads every `/yaffs2/plugins/*.op1p` at boot: code, defaults and screen (the
SVG is inside the file). A plugin file with the name of a built-in replaces it. Maintainers choose per firmware build
which plugins are compiled in and which ship as files: `make_firmware.py --plugin-files a,b` (or `all`).

## Submitting

Open a pull request with your `plugins/<name>/` folder: sources, `plugin.json`, a WAV made with `audio.py` and,
with a screen, a preview PNG. Keep your copyright line in the sources; by submitting you license your plugin under the same terms as op1r
(GPL-3.0-or-later with the OP-1 firmware permission). The maintainers test it on hardware, assign
its number and include it in a release; users can still hide any plugin when they patch their firmware.
