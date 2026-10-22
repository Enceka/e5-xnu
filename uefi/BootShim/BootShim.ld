
/*  Linker script for the E5 boot shim. */

/*  LK loads the boot partition kernel to 0x80080000 and jumps there, so the shim */
/*  is linked at exactly that address and everything is PC relative. */

/*  SPDX-License-Identifier: BSD-2-Clause-Patent */

ENTRY(_Head)

SECTIONS
{
  . = 0x80080000;

  .text : {
    KEEP(*(.text))
  }

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.rela*)
    *(.dynsym)
    *(.dynstr)
  }
}
