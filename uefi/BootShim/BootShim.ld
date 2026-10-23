
/*  Linker script for the E5 boot shim. */

/*  LK loads the boot partition kernel to 0x80080000 and jumps there, so the shim */
/*  is linked at exactly that address and everything is PC relative. */

/*  llvm-objcopy -O binary emits one contiguous image and LK puts the firmware */
/*  volume immediately behind it, so _Payload has to be the last byte of the image: */
/*  its file offset must equal the shim length.  The order below is what makes that */
/*  true, and every input section is named in BootShim.S / BootShimExc.c: */

/*    .shimstart   the shim: _Head (so the Image header sits at 0x80080000), */
/*                 _Start, the vectors, the handler stub and its stack          */
/*    .text.*      BootShimExc.c, one section per function and per object         */
/*    .shimtail   16 bytes of marker, so that the section ends 16 byte aligned */
/*    .shimend    _Payload, empty, at the exact end of the image                 */

/*  SPDX-License-Identifier: BSD-2-Clause-Patent */

ENTRY(_Head)

SECTIONS
{
  . = 0x80080000;

  .text : {
    KEEP(*(.shimstart))
    /*  The exception reporter.  -ffunction-sections -fdata-sections in */
    /*  build-e5.sh keeps every piece of it ahead of _Payload. */
    KEEP(*(.text.*))
    KEEP(*(.rodata))
    KEEP(*(.rodata.*))
    KEEP(*(.data))
    KEEP(*(.data.*))
    KEEP(*(.text))
    KEEP(*(.shimtail))
    /*  No BSS is expected -- the stack and its guard word are PROGBITS inside */
    /*  .shimstart -- but folding a stray one in keeps it from landing past */
    /*  _Payload, where objcopy would pad the image out to it. */
    *(.bss)
    *(COMMON)
  }

  .shimend : {
    KEEP(*(.shimend))
  }

  /DISCARD/ : {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
    *(.rela*)
    *(.dynsym)
    *(.dynstr)
    *(.llvm_addrsig)
    *(.ARM.attributes)
  }
}
