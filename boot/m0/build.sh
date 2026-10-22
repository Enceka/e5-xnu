#!/bin/sh
# Build the M0 bring-up stub. No Xcode, no cross-gcc: clang + ld.lld only.
set -e
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
CC="clang -target aarch64-none-elf -ffreestanding -fno-stack-protector -fno-builtin -mgeneral-regs-only -mstrict-align -O2 -Wall -Wextra"
$CC -c entry.S -o entry.o
$CC -c m0.c -o m0.o
ld.lld -nostdlib -T m0.ld entry.o m0.o -o m0.elf
"$LLVM/llvm-objcopy" -O binary m0.elf m0.bin
"$LLVM/llvm-readelf" -h m0.elf | sed -n '1,20p' | grep -i entry
ls -l m0.bin
