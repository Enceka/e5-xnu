# Build the E5 UEFI firmware and the boot shim that LK can jump to.
#
# Usage:  sh build-e5.sh [DEBUG|RELEASE]
#
# Needs the EDK2 workspace described in docs/uefi-bringup.md: WORKSPACE with a
# Conf directory, mu_basecore, mu_plus and SiliciumPkg reachable through
# PACKAGES_PATH.  The environment is sourced from /tmp/e5uefi/env.sh.
#
# Output:
#   build/SILICIUM_UEFI.fd    raw firmware volume, linked for 0x80200000
#   build/e5-uefi.bin         shim + FV, this is what goes into the boot partition

set -e

MODE=${1:-DEBUG}
FD_BASE=0x80800000
FD_SIZE=0x400000
FD_BLOCKS=0x400
ROOT=`cd \`dirname $0\` && pwd`
LLVM=/opt/homebrew/opt/llvm/bin
OUT=$ROOT/build

mkdir -p $OUT

. /tmp/e5uefi/env.sh

echo "== building EDK2 platform ($MODE) =="
build -a AARCH64 -t CLANGDWARF -b $MODE -p e5Pkg/e5.dsc -n 8 \
  -D FD_BASE=$FD_BASE -D FD_SIZE=$FD_SIZE -D FD_BLOCKS=$FD_BLOCKS

FD=/tmp/e5uefi/Build/e5Pkg/${MODE}_CLANGDWARF/FV/SILICIUM_UEFI.fd
test -f $FD
cp $FD $OUT/SILICIUM_UEFI.fd

echo "== building boot shim =="
$LLVM/clang --target=aarch64-linux-gnu -c $ROOT/BootShim/BootShim.S -o $OUT/BootShim.o \
  -DFD_BASE=$FD_BASE -DFD_SIZE=$FD_SIZE
/opt/homebrew/bin/ld.lld -T $ROOT/BootShim/BootShim.ld -o $OUT/BootShim.elf $OUT/BootShim.o
$LLVM/llvm-objcopy -O binary $OUT/BootShim.elf $OUT/BootShim.bin

echo "== assembling boot image payload =="
/usr/bin/python3 $ROOT/tools/mk-boot-payload.py $OUT/BootShim.bin $OUT/SILICIUM_UEFI.fd $OUT/e5-uefi.bin

ls -l $OUT
