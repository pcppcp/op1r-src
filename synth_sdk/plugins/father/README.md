# Father

Original Hundekraft ambience reverb, inspired by the OP-1 Field Mother workflow. The Father concept and room illustration reference are by xylograph; no xylograph or TE DSP code is included. See `../../../PROVENANCE.md` and the original source notices.

Blue controls predelay/decay, green the duration gate (zero off), white reflection/tail color and orange dry/wet mix. The gate is stereo-linked, refreshes while dry input is active and fades the wet tail without clearing the tank or muting dry sound. Enabling waits for a new attack; disabling opens smoothly.

The tank uses eight unequal delay lines with interleaved normalized four-way Hadamard scattering, independent slow modulation, input allpasses and a crossfed early-reflection path. Color damps both early and late output. `gen_tables.py` derives feedback coefficients from line lengths and target decay.

Callbacks process 64 samples at 22,050 Hz. Synth/drum use mono; master provides separate stereo channels. Per-position state uses the existing FX scratch area. Audio code performs no allocation, file I/O or floating-point work. Delay state belongs to each effect position; controls and mix transitions are smoothed.

References: Julius O. Smith’s FDN Reverberation, TE’s Mother control guide and xylograph’s public Father demonstration. These inform design/appearance, not source-code imports. Licenses and attribution remain in source and provenance.
