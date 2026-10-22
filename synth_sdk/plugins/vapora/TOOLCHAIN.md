# op1r contribution build

Current target: op1r 45, commit `c378c29ddb9c4bcf07491fb056b1a10852c61f43`.
`tools/build.py` invokes the unmodified upstream `make_plugin.py` packer,
using its Docker toolchain or an explicitly supplied native toolchain.
The op1r build uses Blackfin GCC 13.3.0 and binutils 2.42.

The following records the original Hundekraft release environment, for provenance.
The old `.knl` packaging checks are not used by this contribution.

# Pinned development environment

The SDK archive was downloaded from:

- `https://hundekraft.github.io/op1-workshop/source/hundekraft-3.0.2-source.tar.gz`
- Its adjacent published `.sha256` file was downloaded independently and
  checked with `shasum -a 256 -c`.
- Matched SHA-256:
  `35d481d4fd6e3d0e8699bf6fccd8e548c2929ca9155b82b909c175ef24de9700`.
- `PROVENANCE.json`: Kennel commit
  `7c790676ad555da2b8df7a7851826055dee32b2c`.

No existing compiler was found on PATH. An isolated native macOS x86-64
toolchain was built in a dedicated development directory, leaving
existing projects/toolchains unchanged:

| Component | Source | Download SHA-256 |
|---|---|---|
| GCC 13.3.0 | `https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/gcc-13.3.0.tar.xz` | `0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083` |
| binutils 2.42 | `https://ftp.gnu.org/gnu/binutils/binutils-2.42.tar.xz` | `f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800` |

The compiler's own binary hash and version are in `build-record.json`.
GNU source download hashes above record the inputs; only the SDK hash was
compared with the specifically requested published checksum.

`tools/build_toolchain.sh` reproduces the local configuration after putting
the extracted GNU sources in `toolchain-src/` beneath a development directory.
Run it from that development directory. It installs into that directory's
`toolchain/`, not the system. It expects the existing macOS build dependencies
at `/usr/local/opt/{gmp,mpfr,libmpc}` and a C/C++ compiler plus make. Configure
equivalent dependency locations on other hosts.

The source archive's referenced Dockerfile and full emulator harness are
absent. Rather than claim those commands ran, this build uses a freestanding
C-only GCC target with libgcc, a matching assembler/linker and no target libc.
Both builds use system zlib to avoid the bundled old zlib/macOS SDK conflict.
Separate GCC/binutils prefixes require the relative assembler/linker symlinks
made by the script.

The untouched SDK template in `build/scaffold/mysynth/` was packed first:
508 bytes, no imports. Then VAPORA was built with the supplied packer.
No SDK/runtime source was patched.

Host validation used Python 3.14.4, NumPy 2.4.2, Pillow 12.1.1 and Clang's
address/undefined-behavior sanitizers. These do not establish Blackfin
real-time deadlines. The toolchain installation, source downloads and build
logs remain in the workspace; the release archive excludes those large files.
