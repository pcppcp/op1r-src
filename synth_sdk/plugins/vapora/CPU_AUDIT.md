# Current measurement status

See [TESTING.md](TESTING.md) for the October 22 device measurements and op1r
build checks. The audit below is retained as implementation history; references
to pending device measurements and `.knl` packaging describe that earlier stage.

# Oscillator and CPU audit — VAPORA 1.2

The audio implementation described below is unchanged in 1.2. Its expanded
visuals need a combined graphics/audio check on the instrument; host tests
do not establish that performance budget. The following records the earlier
DSP optimizations and their source evidence.

The oscillator count did not fall from multiple to one. R1, R2, R3 and R6 all
initialize `phase[0]` and `phase[1]`; the current `ps_render` advances `ph0` and
`ph1` separately and reads both through `head`. Thickness sets auxiliary level
and detune; Interval sets its pitch ratio. Each head morphs between two related
PADsynth endpoints. One waveform can contain many broadened/detuned spectral
components. Spectral components are not additional runtime oscillators.

| Revision | Relevant cost changes | Playback heads per voice |
|---|---|---|
| R1 | Full-rate table/filter path and repeated nested table interpolation | 2 |
| R2 | Half-rate synthesis/filtering; block-resolved pointers; reconstructed output | 2 |
| R3 | Guarded single mip at rest; shared transition fade; block LFO/coefficient ramps; constant-pitch path; inlined reads; symmetric FIR; staggered telemetry | 2 |
| R6 | More source spectra; compact bank index mask | 2 |
| R7 | Same production DSP source; a more focused table bank and preset decoding | 2 |

Current six-voice steady-state cost includes 3,072 table sample reads per
128-frame output block (64 internal steps × two heads × four reads × six
voices). During an active bank transition it is 6,144. Four reads are two
linearly interpolated Morph endpoints. The two-head architecture and existing
interpolation remain; the optimization avoided doing unnecessary extra bank
interpolations continuously.

The pinned community sources were inspected again:

- `synth_sdk/plugins/eva/eva.c`: two oscillator choir; 22.05 kHz internal audio,
  interpolated output and block-prepared/ramped controls. Shows half-rate pads
  can retain multiple oscillators.
- `synth_sdk/plugins/oxbow/oxbow.c` and `obv_dsp.h`: two oscillators at half rate,
  cached block settings and reciprocal calculations, four-point output
  reconstruction. Its per-block oscillator passes are useful for its own
  sync/X-mod design; importing that algorithm would change VAPORA unnecessarily.
- `synth_sdk/plugins/wavetable/wavetable_dsp.h`: inline interpolated reads and
  fixed-point oscillator/filter path, matching the current implementation approach.
- `synth_sdk/plugins/elg/README.md`: three detuned oscillators. This is evidence
  that multiple oscillators are possible, not that a third VAPORA head is free.

R7 keeps the proven two heads. Adding another full head increases table work;
there is no measured device headroom budget supporting that change. Instead,
seven detuned Gaussian lobes per partial are baked into the new PADsynth spectra
on the computer, with no extra real-time oscillators or FFTs. Warm retains the
original two long tables. Six cinematic endpoints now use 8,192 samples instead
of R6's common 2,048, trading source variety for richer loop detail within the
512 KiB package limit.

Source equality and retained-sound PCM equality are recorded separately.
Host benchmarks exclude GUI, adapter, T2, effects and Blackfin memory behavior.
User feedback supports an earlier CPU improvement, but not a controlled
attribution of how much each change saved. Device AUDIO peak remains the
acceptance test, especially with Reverb and overlapping release tails.

PADsynth author: https://zynaddsubfx.sourceforge.io/doc/PADsynth/PADsynth.htm
Community source: pinned Hundekraft 3.0.2 / Kennel commit
`7c790676ad555da2b8df7a7851826055dee32b2c`.
