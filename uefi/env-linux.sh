#!/bin/sh
# EDK2 build environment for the E5 (Unisoc UMS9621) firmware on Linux.
#
# Nothing is installed into the system: the iasl/nasm/libuuid RPMs are unpacked
# under $E5_DEPS/local and put on PATH/CPATH/LIBRARY_PATH instead.  See
# docs/build-linux.md for how that directory is produced from scratch.
#
# Use it either directly
#   E5_ENV=/home/hema/Workspace/e5-xnu/uefi/env-linux.sh sh uefi/build-e5.sh DEBUG
# or as the well-known copy that build-e5.sh falls back to
#   cp uefi/env-linux.sh ~/e5uefi/env.sh
#
# $E5_DEPS  where the dependency repos and the extracted RPMs live
# $E5_REPO  this repository (build-e5.sh sets it to its own root)

E5_DEPS=${E5_DEPS:-$HOME/e5uefi}
E5_REPO=${E5_REPO:-$HOME/Workspace/e5-xnu}

# EDK2 workspace: holds BaseTools (a symlink to mu_basecore is enough), Conf/
# and Build/.
export WORKSPACE=${WORKSPACE:-$E5_DEPS/work}
export EDK_TOOLS_PATH=$WORKSPACE/BaseTools
export EDK_TOOLS_BIN=$EDK_TOOLS_PATH/Source/C/bin

# EDK2 needs Conf/ next to WORKSPACE.  edksetup.sh normally seeds it from the
# templates; do the same here so sourcing this file is the only setup step.
# Never overwrite: Conf/ is where local tweaks live.
if [ -d "$EDK_TOOLS_PATH/Conf" ]; then
  mkdir -p "$WORKSPACE/Conf"
  for t in target tools_def build_rule; do
    [ -f "$WORKSPACE/Conf/$t.txt" ] || cp "$EDK_TOOLS_PATH/Conf/$t.template" "$WORKSPACE/Conf/$t.txt"
  done
fi

# Every package root the platform DSC refers to.  A missing entry makes "build"
# print "- Failed -" with no error message at all, so the list has to be
# complete: mu_basecore (probe), mu_plus, mu_oem_sample, Silicium and the two
# E5 packages.
export PACKAGES_PATH=$WORKSPACE\
:$E5_DEPS/probe\
:$E5_DEPS/mu_plus\
:$E5_DEPS/mu_oem_sample\
:$E5_DEPS/uefi-sheng/Silicon/Silicium\
:$E5_REPO/uefi/Platforms/Unisoc\
:$E5_REPO/uefi/Silicon/Unisoc

# clang/llvm for CLANGDWARF.  On Fedora clang, llvm-ar, llvm-objcopy, llvm-rc
# and ld.lld all live in /usr/bin; the macOS layout (LLVM prefix plus a separate
# bin/ for ld.lld) is the fallback build-e5.sh uses when this file is absent.
export CLANGDWARF_BIN=${CLANGDWARF_BIN:-/usr/bin/}
export CLANG_BIN=${CLANG_BIN:-/usr/bin/}

# iasl/nasm from the unpacked RPMs, the BaseTools Python wrappers and the C
# tools built from mu_basecore.
export PATH=$E5_DEPS/local/usr/bin:$EDK_TOOLS_BIN:$EDK_TOOLS_PATH/BinWrappers/PosixLike:$PATH

# BaseTools links against libuuid; the header and libuuid.so come from
# libuuid-devel unpacked under $E5_DEPS/local as well.
export CPATH=$E5_DEPS/local/usr/include${CPATH:+:$CPATH}
export LIBRARY_PATH=$E5_DEPS/local/usr/lib64${LIBRARY_PATH:+:$LIBRARY_PATH}
export EXTRA_LDFLAGS="-L$E5_DEPS/local/usr/lib64${EXTRA_LDFLAGS:+ $EXTRA_LDFLAGS}"

# The BinWrappers exec "python" unless PYTHON_COMMAND is set; Fedora only ships
# python3.
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}

export E5_FD_BASE=${E5_FD_BASE:-0x80800000}
export E5_FD_SIZE=${E5_FD_SIZE:-0x400000}
export E5_FD_BLOCKS=${E5_FD_BLOCKS:-0x400}
