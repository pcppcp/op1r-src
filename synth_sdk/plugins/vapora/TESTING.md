# VAPORA 1.2.3 validation — 2026-10-22

## OP-1 hardware: Hundekraft 3.0.2

User-reported resource monitor readings on an original OP-1 running VAPORA
1.2.3, A Fifth Tide, T3 effect off and tape stopped. MIDI channel 1,
velocity 100; held notes were C3, G3, C4, E4, G4, C5 (MIDI 48, 55, 60, 64,
67, 72), using the first 1, 3 or 6 notes. Holds were 15 seconds with
10-second release gaps.

| Held notes | CPU | AUDIO |
|---|---:|---:|
| None | 39% | 16% |
| 1 | 47% | 26% |
| 3 | 56% | 38% |
| 6 | 76% | 54% |

These are whole-system monitor readings, not isolated plugin cost. The monitor
replaces VAPORA's animated screen, so they do not measure that screen's cost.
They are individual reported readings, not peak/percentile measurements.

In separate playing with the visualizer visible, the user reported no audible
breakup or apparent max-out with 7–8 keys and then 10 keys held. VAPORA still
has six synthesis voices; additional keys exercise voice stealing. This is a
qualitative playing observation, not a ten-voice benchmark or proof of zero
sample clipping. Tape-plus-FX worst-case load has not been measured.

## op1r contribution checks

Target: op1r 45, `c378c29ddb9c4bcf07491fb056b1a10852c61f43`.

- Standard `make_plugin.py` packer, with native Blackfin GCC 13.3.0/binutils
  2.42: `.op1p` 483,501 bytes, image 472,908 bytes, BSS 4,024 bytes;
  624 relocations. Relocation self-check against GNU ld at two addresses
  passes; all three imports are exported by this core (`sq_cat`, `sq_num`,
  `op1_eng_page`). The runtime table samples account for 448,800 bytes.
- Standard upstream `harness/audio.py vapora`: ASan/UBSan pass, audible
  nonzero dry output, peak 6,217 (-14.4 dBFS) in this specific fixture.
  `audio_vapora.wav` is its unnormalized output.
- Upstream `screen_harness.c` compiled and run with ASan/UBSan: passes,
  249 raster lines in the sampled default frame. The usual Cairo preview
  renderer was not available locally; `preview.png` is rendered from the
  VAPORA host harness's actual SVG commands using Pillow and a substitute font.
- VAPORA host suite: DSP, adapter, mailbox and screen checks pass with
  ASan/UBSan, including three simulated minutes of six-voice sustained state,
  Shift behavior, overlapping note impacts, all seven suns, interval star
  fields and wave bounds. Table generation repeated with identical hashes;
  15 factory AIFC presets regenerated and round-trip checked; 19 dry audio
  fixtures checked for signal and spectral limits.
- Earlier 1.2.3 dry output-level sweep: 13,440 six-voice cases, largest absolute
  output 26,243/32,767 (about 1.9 dB headroom), no clipped samples. Native DSP
  is unchanged by the op1r adaptation. This does not cover all possible
  phase combinations or downstream processing.

No on-device op1r 45 load/play test has been performed. Remaining checks:
plugin loading, control/Shift behavior, preset import and save/reload, FX
mapping and missing-effect handling, animated-screen cost, and tape plus FX
under load. Deep Pressure requests Reverb. Stone Hall has been regenerated
with T3 off for this contribution, preserving all eight sound words, ADSR and
octave -3; its AIFC payload was round-trip checked. The other thirteen factory
recipes are also dry.

## Performance design and limits

PADsynth spectra are generated offline. Runtime synthesis uses fixed-point
arithmetic, two independently phased table heads per voice, half-rate synthesis
with reconstruction, block preparation and guarded transitions. It performs
no runtime FFT or dynamic allocation. The screen caches geometry at roughly
16 updates/second at the expected 400 MHz clock; raster commands are still
submitted every display frame. Update throttling alone is not a measurement
of rendering cost. Host timing and draw-command counts cannot establish a
Blackfin CPU advantage over another plugin.
