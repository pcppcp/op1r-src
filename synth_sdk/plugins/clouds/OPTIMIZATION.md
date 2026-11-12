# Clouds exact optimization

This change fuses full-rate grain envelope/read work, caches grain setup by its
control inputs, hoists invariant seeding comparisons and specializes delay-line
addressing. Grain count, audio rate, RNG sequence and per-sample mix remain intact.
Both `clouds_dsp.c` and its header must be rebuilt together.

## Reproduce the output comparison

From the repository root:

```sh
python3 synth_sdk/harness/clouds_exact_verify.py .
```

Requires Python 3, Git and a C compiler with ASan and UBSan. Set `CC` to select the
compiler. The base revision defaults to
`358d7e6d0460318e1108784fe14acfbfd3850ab4`; `--base` selects another unoptimized
revision. The script builds in a temporary directory without changing the tree.

Six paired granular-mode captures pass sample-for-sample with ASan, UBSan and
`CLOUDS_CHECK`: each of the OP-1 (44.1 kHz, 24 grains, linear reads, idle skipping)
and reference (32 kHz, 40 grains, Hermite reads) configurations runs:

- 12,000 changing-control blocks, lengths 1–64, across three instances, with
  freeze/trigger, reinitialization and pitch changes.
- 24,000 sustained positive/negative/alternating/saw rail blocks, full reverb,
  freeze transitions and pitch at ±24 semitones.
- 100 blocks starting near `INT32_MAX`, comparing both audio and phasor state.

The baseline uses `-fwrapv` to define the inherited signed phasor overflow;
the patch uses unsigned addition and runs without `-fwrapv`. Feedback stays zero
as in the OP-1 wrapper. These checks do not replace the broader existing
float-reference/looping-delay harness. Hashes below are from little-endian captures.

## Blackfin instruction-work measurements

Compiler: BF524 GCC 13.3, `-O2`. The same loader, mono wrapper, input and raw control
words are used before/after. Work is the difference between cumulative runs of
2,756 and 3,012 renders, divided by 256 (64 samples per render). Thus initialization
and early grain buildup are excluded from the measured interval.

These are executed instruction packets, **not physical cycles or sysmon CPU
percentages**. Cache/SDRAM stalls are absent. This is a controlled probe
using an equivalent mono wrapper, not a comparison of complete firmware.
Physical OP-1 acceptance is still needed; no deadline guarantee is claimed.

| Probe case | Upstream packets/render | Patched | Reduction |
|---|---:|---:|---:|
| A: baseline texture | 32,293.4 | 28,828.2 | 10.7% |
| B: maximum controls | 147,397.5 | 122,065.9 | 17.2% |
| C: diffuse texture | 111,517.9 | 89,133.2 | 20.1% |
| D: dense + reverb | 143,598.6 | 117,706.8 | 18.0% |

The case names are probe identifiers, not factory presets. These are the exact
input parameter words **before** the wrapper's ordinary parameter normalization
and clamping. Word 4 is the plugin marker and is not a DSP control.

| Case | p0 | p1 | p2 | p3 | p5 | p6 | p7 |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | 16384 | 16384 | 24576 | 16384 | 16384 | 22938 | 0 |
| B | 32767 | 32767 | 32767 | 32767 | 32767 | 32767 | 32767 |
| C | 20000 | 30000 | 2000 | 30000 | 4000 | 32767 | 0 |
| D | 8000 | 32767 | 0 | 0 | 32767 | 32767 | 32767 |

## Capture hashes at the time of the change

For reference only: they change with any later edit of the clouds DSP. `clouds_exact_verify.py` does not use
them; it compares the patched tree with the base revision directly.

| Configuration | Scenario | Bytes | SHA-256 |
|---|---|---:|---|
| op1 | dynamic | 790680 | `b6181e7578bd0f1865c665ac0a4c82c62afa2e5930eb3d28f9d275db248c4452` |
| op1 | rails | 3072000 | `f9b8bcf561c2b35754a6bef72cbd2b053c882f5ef5e9349b16da66d8690bbc8f` |
| op1 | edge | 13200 | `784e6df1b4ff4afa9b57407c74c03b14c523a3b92c2fe6b3b542b7f126dc0555` |
| reference | dynamic | 790680 | `3f83b142b6a36036783f465d00849a560def7f399a87ef1c9d09530fc414069b` |
| reference | rails | 3072000 | `3d01158fec803fbbed8ed771ec78d6f60468bc6ea651dde1864fbe654ba590fb` |
| reference | edge | 13200 | `784e6df1b4ff4afa9b57407c74c03b14c523a3b92c2fe6b3b542b7f126dc0555` |

## Dry end (2026-11-12, op1r-src PR #40 by Hundekraft, dry end only)

With MIX fully dry the effect was about 9 dB quieter than FX off: the original's equal-power dry gain (0.707 at mix 0)
and its halving before the soft limiter (SoftConvert = SoftLimit(x * 0.5)). The dry gain is now the curve x 2 sqrt 2
(2.0 at mix 0, unity after the halving); the wet side and the halving are as in the original, so MIX fully wet keeps
its level (the middle of MIX has more dry than before: mix 0.5 -4.0 -> -0.7 dB vs FX off). `CLOUDS_ORIGINAL_DRY`
restores the original dry gain; `harness/clouds_compare.py` builds with it (11 / 11 against the original), and
`harness/clouds_mix_test.c` checks the levels. The capture hashes above are from before this change.
