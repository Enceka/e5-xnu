# Build the E5 UEFI firmware and the boot shim that LK can jump to.
#
# Usage:  sh build-e5.sh [DEBUG|RELEASE]
#
# Needs the EDK2 workspace described in docs/uefi-bringup.md: WORKSPACE with a
# Conf directory, mu_basecore, mu_plus and SiliciumPkg reachable through
# PACKAGES_PATH.  The environment comes from $E5_ENV; without it the script
# looks for /tmp/e5uefi/env.sh (the macOS workspace), then
# $HOME/e5uefi/env.sh, then env-linux.sh next to this script (the Linux setup
# in docs/build-linux.md).
#
# Output:
#   build/SILICIUM_UEFI.fd    raw firmware volume, linked for $FD_BASE
#   build/e5-uefi.bin         shim + FV, this is what goes into the boot partition

set -e

MODE=${1:-DEBUG}

# SELFTEST=1 builds a shim that arms the watchdog and then parks instead of
# entering SEC.  One boot then answers two questions at once: did LK really
# jump to 0x80080000, and does the dead man's switch reset the board.
# Extra flags handed to the assembler, e.g. SHIM_FLAGS="-DNO_WATCHDOG -DSHIM_SELFTEST".
SHIM_EXTRA=${SHIM_FLAGS:-}
if [ "${SELFTEST:-0}" = "1" ]; then
  SHIM_EXTRA="$SHIM_EXTRA -DSHIM_SELFTEST"
  echo "== SELFTEST build: shim parks after arming the watchdog =="
fi
if [ -n "$SHIM_EXTRA" ]; then
  echo "== shim flags:$SHIM_EXTRA"
fi
FD_BASE=${E5_FD_BASE:-0x80800000}
FD_SIZE=${E5_FD_SIZE:-0x400000}
FD_BLOCKS=${E5_FD_BLOCKS:-0x400}
ROOT=`cd \`dirname $0\` && pwd`
OUT=$ROOT/build

# The environment script uses E5_REPO to find the E5 packages inside this repo.
E5_REPO=${E5_REPO:-$ROOT/..}
export E5_REPO

# The EDK2 environment comes first: it is what supplies WORKSPACE and the
# CLANGDWARF toolchain directory, so the defaults below only apply when the
# environment does not say otherwise.  The fallbacks are the macOS paths, which
# is where this script was originally written.
#
# E5_ENV points at that file.  The default is the macOS workspace; a Linux box
# keeps the same layout under $HOME and can also use the copy that ships with
# the repo (env-linux.sh, see docs/build-linux.md).
ENV_SH=${E5_ENV:-/tmp/e5uefi/env.sh}
if [ ! -f "$ENV_SH" ]; then
  ENV_SH="${E5_DEPS:-$HOME/e5uefi}/env.sh"
fi
if [ ! -f "$ENV_SH" ]; then
  ENV_SH="$ROOT/env-linux.sh"
fi
if [ ! -f "$ENV_SH" ]; then
  echo "== no EDK2 environment found: set E5_ENV=/path/to/env.sh ==" >&2
  exit 1
fi
echo "== environment: $ENV_SH"
. "$ENV_SH"

LLVM=${E5_LLVM_BIN:-${CLANGDWARF_BIN:-/opt/homebrew/opt/llvm/bin}}
LLVM=${LLVM%/}
# ld.lld is not in the Homebrew llvm prefix, it lives in bin/ next to it.
LD_LLD=${E5_LD_LLD:-`command -v ld.lld || echo /opt/homebrew/bin/ld.lld`}
# The build script and mk-boot-payload.py are Python 3; some systems only ship
# the versioned name, so ask the environment first.
PYTHON=${E5_PYTHON:-`command -v python3 || echo /usr/bin/python3`}
BUILD_DIR=${E5_BUILD_DIR:-$WORKSPACE/Build/e5Pkg/${MODE}_CLANGDWARF}

mkdir -p $OUT

echo "== building EDK2 platform ($MODE) =="
build -a AARCH64 -t CLANGDWARF -b $MODE -p e5Pkg/e5.dsc -n 8 \
  -D FD_BASE=$FD_BASE -D FD_SIZE=$FD_SIZE -D FD_BLOCKS=$FD_BLOCKS

FD=$BUILD_DIR/FV/SILICIUM_UEFI.fd
test -f $FD
cp $FD $OUT/SILICIUM_UEFI.fd

echo "== building boot shim =="
$LLVM/clang --target=aarch64-linux-gnu -c $ROOT/BootShim/BootShim.S -o $OUT/BootShim.o \
  -DFD_BASE=$FD_BASE -DFD_SIZE=$FD_SIZE $SHIM_EXTRA
# The exception reporter.  -mgeneral-regs-only because a fault can be taken
# before SEC has enabled FP/SIMD, so the handler may not touch those registers.
# -ffreestanding/-nostdlib because there is no libc at 0x80080000; the file
# carries its own memset-free helpers.  Built -fno-pic so that no relocation
# survives into the fixed-address image.
# -mstrict-align: the handler runs with the MMU off, so every access is Device
# memory and an unaligned store is an alignment fault.  Without this the
# compiler emits "stur x8, [sp, #4]" for the record buffer and the handler
# faults on its own stack -- reproduced in qemu.
# -ffunction-sections/-fdata-sections: BootShim.ld has to be able to place every
# piece of it ahead of _Payload, which must stay the last byte of the image.
$LLVM/clang --target=aarch64-linux-gnu -c $ROOT/BootShim/BootShimExc.c \
  -o $OUT/BootShimExc.o -ffreestanding -nostdlib -fno-pic -fno-stack-protector \
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
  -mstrict-align -mgeneral-regs-only -O2 \
  -I$ROOT/Silicon/Unisoc/UMS9621Pkg/Library/SprdDebugSerialPortLib
$LD_LLD -T $ROOT/BootShim/BootShim.ld -o $OUT/BootShim.elf $OUT/BootShim.o $OUT/BootShimExc.o
$LLVM/llvm-objcopy -O binary $OUT/BootShim.elf $OUT/BootShim.bin

echo "== assembling boot image payload =="
$PYTHON $ROOT/tools/mk-boot-payload.py $OUT/BootShim.bin $OUT/SILICIUM_UEFI.fd $OUT/e5-uefi.bin

ls -l $OUT
