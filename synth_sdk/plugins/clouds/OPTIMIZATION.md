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
| op1 | dynamic | 790680 | `13870aa389605aa45d6388dbd32266257b8a6b0bf8e2469560990e6bc9208940` |
| op1 | rails | 3072000 | `66a49d757e0a636468631bd97aa79dab708a799e97667c59b7815144dc25be4d` |
| op1 | edge | 13200 | `784e6df1b4ff4afa9b57407c74c03b14c523a3b92c2fe6b3b542b7f126dc0555` |
| reference | dynamic | 790680 | `56408c2b161be3fd0b30930e6e444bd11a5b413de5a587858eaf2c841c485766` |
| reference | rails | 3072000 | `e189679dfcc57e20f439a4af8abada1e24efa93cc1c50cbb31da72a161e1f542` |
| reference | edge | 13200 | `784e6df1b4ff4afa9b57407c74c03b14c523a3b92c2fe6b3b542b7f126dc0555` |

## Dry end (2026-11-10)

The final mix used to halve the sum before the soft limiter, and the dry gain was the bare equal-power curve (0.707 at
mix 0), so switching Clouds on with MIX fully dry dropped the level by about 9 dB. The dry gain is now the curve x sqrt 2
(unity at mix 0), the wet gain is 1.2 x sqrt 2 (55609 in Q15, was 39322), and the sum goes to the limiter unhalved. Both
ends are 2.83 x the old gains, so the balance across MIX is as before. The hashes above are for this version, and
`clouds_exact_verify.py` no longer matches its base revision in the dry / wet stage on purpose.
