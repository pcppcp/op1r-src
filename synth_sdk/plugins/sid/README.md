# SID

Six-voice OP-1 synth using selected oscillator/filter primitives and 8580 tables from cRSID 1.58 by Hermit (Mihaly Horvath). The original permissive notice remains in source. It includes no C64 CPU/tune player and makes no cycle-exact or 6581-model claim.

Each voice owns three oscillators and a resonant low-pass filter. Blue selects triangle, saw, pulse, noise, sync or ring. Green changes voice blend, pulse width or oscillator ratio according to mode. White controls cutoff and orange resonance.

Cutoff follows a logarithmic, interpolated coefficient sweep, with per-sample smoothing; the first note starts at the requested coefficient. The display is control position, not Hz. The stock T2 envelope supplies amplitude.

Keep per-voice state bounded and preserve the cRSID copyright/license notice. Generated artwork uses independent native-safe contours and the shared line-weight/placement helpers. Source: `https://hermit.cucumalac.hu/cRSID-1.58-with-source-code.tar.gz`.

Original cRSID README notice (also retained in the adapted source/table headers):

> License is still WTF: Do what the frick you want with this code, but
> it would be nice mentioning me as the original author.
