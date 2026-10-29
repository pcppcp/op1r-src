# Licenses and provenance

- **PADsynth algorithm:** Nasca Octavian Paul,
  [author's explanation and public-domain reference](https://zynaddsubfx.sourceforge.io/doc/PADsynth/PADsynth.htm).
  The Gaussian spectral-broadening procedure is implemented here in Python;
  no padthv1 application code or FFTW code is included.
- **Kennel/OP-1 Workshop:** Hundekraft, pcppcp, ultralazr and contributors.
  SDK 3.0.2, Kennel commit `7c790676ad555da2b8df7a7851826055dee32b2c`.
  Integration follows its synth template and Oxbow, EVA, MBIRA, Aether, SID,
  Father and Wavetable references. Preset container code is adapted from
  Hundekraft's EVA/MBIRA `presets.py`. See the copied `LICENSE` and `NOTICE`:
  GPL-3.0-or-later with the existing additional OP-1 firmware combination
  permission. Retain upstream notices when redistributing.
- **VAPORA source, generators, presets and original coastal artwork:**
  2026 VAPORA contributors, GPL-3.0-or-later with that OP-1 firmware permission.
  Generated numerical tables are distributed on those same terms.
- **Host FFT dependency:** NumPy 2.4.2, BSD-3-Clause (with its own bundled
  component notices). Installed as a host dependency, not linked into the
  plugin. [NumPy license](https://numpy.org/doc/stable/license.html).
- **Host preview dependency:** Pillow 12.1.1, HPND/PIL license and component
  notices. Host-only. [Pillow license](https://pillow.readthedocs.io/en/stable/about.html#license).
- **Compiler:** GCC 13.3.0, GPLv3; linked libgcc runtime portions carry the
  GCC Runtime Library Exception as supplied by GCC. GNU binutils 2.42 is
  a build tool. Neither compiler executable nor toolchain sources are
  bundled in the small delivery archive.
- Runtime lettering calls the stock firmware text renderer through SDK wrappers. The source archive
  contains no extracted TE stock font, original firmware, device keys,
  proprietary artwork or third-party reference JPG.

The PADsynth author's public-domain dedication does **not** make the combined
plugin, SDK-derived code, NumPy, or other components public domain.
