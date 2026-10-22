#!/bin/sh
# Build the M0 stub. clang + ld.lld only. Extra defines: EXTRA=-DM0_FB sh build.sh
set -e
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
CF="-target aarch64-none-elf -ffreestanding -fno-stack-protector -fno-builtin -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra"
clang $CF $EXTRA -c entry.S -o entry.o
clang $CF $EXTRA -c m0.c -o m0.o
ld.lld -nostdlib -T m0.ld entry.o m0.o -o m0.elf
"$LLVM/llvm-objcopy" -O binary m0.elf m0.bin
ls -l m0.bin
