#!/bin/sh
# Run from an isolated development directory with extracted toolchain-src/.
# GPL-3.0-or-later with OP-1 firmware permission.
set -eu
TASK_ROOT=$(pwd)
mkdir -p toolchain/build-binutils toolchain/build-gcc logs
cd "$TASK_ROOT/toolchain/build-binutils"
../../toolchain-src/binutils-2.42/configure --target=bfin-elf \
  --prefix="$TASK_ROOT/toolchain/bfin" --disable-nls --disable-werror \
  --disable-gdb --disable-sim --disable-gprofng --with-system-zlib \
  > "$TASK_ROOT/logs/binutils-configure.log" 2>&1
make -j8 MAKEINFO=true > "$TASK_ROOT/logs/binutils-build.log" 2>&1
make install MAKEINFO=true > "$TASK_ROOT/logs/binutils-install.log" 2>&1
cd "$TASK_ROOT/toolchain/build-gcc"
PATH="$TASK_ROOT/toolchain/bfin/bin:$PATH"
export PATH
../../toolchain-src/gcc-13.3.0/configure --target=bfin-elf \
  --prefix="$TASK_ROOT/toolchain/bfin-gcc" --enable-languages=c \
  --without-headers --disable-bootstrap --disable-nls --disable-shared \
  --disable-threads --disable-libssp --disable-libquadmath --disable-libgomp \
  --disable-libatomic --disable-multilib --with-newlib --with-system-zlib \
  --with-gmp=/usr/local/opt/gmp --with-mpfr=/usr/local/opt/mpfr \
  --with-mpc=/usr/local/opt/libmpc > "$TASK_ROOT/logs/gcc-configure.log" 2>&1
make -j8 all-gcc all-target-libgcc MAKEINFO=true > "$TASK_ROOT/logs/gcc-build.log" 2>&1
make install-gcc install-target-libgcc MAKEINFO=true > "$TASK_ROOT/logs/gcc-install.log" 2>&1
mkdir -p "$TASK_ROOT/toolchain/bfin-gcc/bfin-elf/bin"
ln -sf ../../../bfin/bin/bfin-elf-as "$TASK_ROOT/toolchain/bfin-gcc/bfin-elf/bin/as"
ln -sf ../../../bfin/bin/bfin-elf-ld "$TASK_ROOT/toolchain/bfin-gcc/bfin-elf/bin/ld"
"$TASK_ROOT/toolchain/bfin-gcc/bin/bfin-elf-gcc" --version
