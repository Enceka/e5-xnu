//
//  Boot shim for the Rongyue E5 (Unisoc UMS9621).
//
//  LK, the stock Android bootloader, always copies the boot partition kernel to
//  0x80080000 and jumps there with
//
//    x0 = physical address of the merged vendor FDT (0x86000000 on this board)
//    MMU off, caches off, EL2
//
//  EDK2 wants to live at PcdFdBaseAddress (0x80800000 for the E5), so this shim
//  sits at 0x80080000, copies the firmware volume that follows it up to
//  0x80800000, and branches to the FV base.  The FV zero vector there is a
//  branch to SEC, which picks up x0 and stashes it in the
//  gSiliciumEntryArgHobGuid HOB for DtPlatformDxe.
//
//  The first 64 bytes double as a valid AArch64 Image header.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

.set SHIM_BASE, 0x80080000

//
//  Stage markers on the panel.
//
//  The framebuffer is the only channel readable without rebooting: LK leaves
//  the DPU scanning out 0xbe000000 at 320x480 RGB565 with 640 byte rows and the
//  backlight on before it jumps to us.  M0 proved a full screen paint from bare
//  metal is visible, so every stage fills the WHOLE screen.  A 16 pixel strip
//  at the top was tried first and was indistinguishable from a dark panel.
//
//  The last colour on screen is the last stage reached:
//
//    blue     0x001f  the shim was entered at all (LK jumped to 0x80080000)
//    yellow   0xffe0  the source was cleaned to the point of coherency
//    green    0x07e0  the firmware volume was copied to FD_BASE
//    magenta  0xf81f  the copy was published (ic iallu)
//    white    0xffff  the watchdog is armed and SEC is being entered
//    blinking red/green  selftest only: parked, panel and CPU both alive
//
//  If SEC runs it clears the display and the text console takes over.
//
.macro FBFILL color
  ldr   x0, =0xbe000000
  ldr   x2, =153600             // 320 * 480 pixels
991:
  strh  w1, [x0], 2
  subs  x2, x2, 1
  b.ne  991b
.endm

//  Same, with the colour loaded first (the fill loop above assumes w1).
.macro FBSET color
  mov   w1, #(\color)
  FBFILL \color
.endm

//  How long the selftest parks before falling through into SEC, in units of
//  one red/green cycle (0.8 s).
#define SHIM_BLINKS 300

//  Busy wait, in milliseconds, off the 26 MHz virtual counter.
.macro FBDELAY ms
  mov   x4, #26000              // ticks per millisecond
  mov   x5, #(\ms)
  mul   x4, x4, x5
  mrs   x3, cntvct_el0
  add   x4, x4, x3
992:
  mrs   x3, cntvct_el0
  cmp   x3, x4
  b.lo  992b
.endm

//  Point VBAR at a 2 KiB aligned table.  VBAR_EL2 is only writable at EL2 or
//  above; VBAR_EL1 is writable at EL2 too, so writing it is always safe.
.macro SETVBAR tbl
  adrp  x9, \tbl
  add   x9, x9, :lo12:\tbl
  mrs   x10, currentel
  and   w10, w10, #0xc
  cmp   w10, #0x8                // CurrentEL == EL2 ?
  b.ne  88f
  msr   vbar_el2, x9
88:
  msr   vbar_el1, x9
  isb
.endm

//  Three sections on purpose, because BootShim.ld orders them around the C
//  object:
//
//    .shimstart  everything that must sit at 0x80080000 -- the Image header
//                (_Head), _Start, the vectors, the handler stub and its stack
//    .shimtail   a 16 byte end marker, placed after the C object
//    .shimend    _Payload, placed last so that it is the final byte of the image
//                (LK puts the firmware volume right behind the shim, so its file
//                offset has to equal the shim length)
//
//  All three are PROGBITS: llvm-objcopy -O binary does not emit NOBITS, so a
//  .bss stack or a .bss marker would silently shrink the image.
//
.section .shimstart, "ax", %progbits
.global _Head
.global _Start

.balign 4096
_Head:
  b     _Start                  // code0: branch to the real entry
  .long 0                       // code1: reserved
  .quad 0x80000                 // text_offset
  .quad 0                       // image_size, patched by mk-boot-payload.py
  .quad 0                       // flags
  .quad 0                       // reserved
  .quad 0                       // reserved
  .quad 0                       // reserved
  .ascii "ARM"                  // magic, low half
  .byte 0x64                    // magic, high byte: "ARMd"
  .long 0                       // reserved

_Start:
  // Mask everything: LK may hand over with interrupts live, and its EL2
  // vectors are still installed.  M0 did this first thing and never faulted.
  msr   daifset, #0xf

  // Install our own vectors before anything else can fault.
  //
  // Nothing in Silicium ever writes VBAR: SecVectorTable exists in
  // SiliciumPkg/Sec/AArch64/Exception.S but no code loads it, so every
  // exception taken inside SEC lands on LK's leftover EL2 vectors and LK's
  // panic path resets the board with no trace.  Installing them here means
  // they stay live from the first instruction of this shim until DXE core
  // installs its own, which covers exactly the window we are chasing.
  //
  // The chain hands over at EL2, but the check is free and VBAR_EL2 is
  // undefined at EL1: writing it there would fault and recurse.
  SETVBAR _VectorBase

  // Save the FDT pointer; the copy loop below clobbers x1..x10.
  mov   x19, x0

  FBSET 0x001f                  // blue: shim entered

  // Source is the payload that LK loaded right after this shim.
  adr   x4, _Payload
  ldr   x5, =FD_BASE
  ldr   x6, =FD_SIZE

  // Make the source visible: clean and invalidate it to the point of coherency.
  mrs   x9, ctr_el0
  ubfx  x9, x9, 16, 4           // DminLine, log2 words
  mov   x10, 4
  lsl   x9, x10, x9             // cache line size in bytes
  sub   x10, x9, 1

  bic   x11, x4, x10
  add   x12, x4, x6
_SrcClean:
  dc    civac, x11
  add   x11, x11, x9
  cmp   x11, x12
  b.lo  _SrcClean
  dsb   sy

  FBSET 0xffe0                  // yellow: source cleaned

  // Copy the firmware volume to FD_BASE.
  mov   x7, x5
  mov   x8, x4
_CopyLoop:
  ldp   x2, x3, [x8], 16
  stp   x2, x3, [x7], 16
  subs  x6, x6, 16
  b.ne  _CopyLoop

  FBSET 0x07e0                  // green: firmware volume copied

  // Publish the copied instructions and data.
  ldr   x5, =FD_BASE
  ldr   x6, =FD_SIZE
  bic   x11, x5, x10
  add   x12, x5, x6
_DstClean:
  dc    civac, x11
  add   x11, x11, x9
  cmp   x11, x12
  b.lo  _DstClean
  dsb   sy
  ic    iallu
  dsb   sy
  isb

  FBSET 0xf81f                  // magenta: copy published

  // Arm the AP watchdog as a dead man's switch before anything risky runs.
  //
  // A wedged loop never takes an exception, so LK's panic handler (which does
  // reset the SoC) never runs; without a watchdog the board stays dead until
  // the battery is pulled.  Measured the hard way.
  //
  // The AON register block is clock gated out of reset.  Both APB clocks are
  // "set" gates in the aonapb controller at 0x64900000 (syscon@64900000,
  // aonapb-gate, "sprd,ums9621-aon-gate"): write the mask to
  // base + reg + 0x1000 to enable.
  //   ap-wdg-eb     reg 0x08 bit 29  -> 0x64901008
  //   ap-wdg-rtc-eb reg 0x18 bit 7   -> 0x64901018
#ifdef NO_WATCHDOG
  b     _EnterSec
#endif

  ldr   x0, =0x64901008
  mov   w1, #0x20000000
  str   w1, [x0]
  ldr   x0, =0x64901018
  mov   w1, #0x80
  str   w1, [x0]
  dsb   sy

  // AP watchdog at 0x641e0000, sprd,wdt-r2p0-fiq.
  //   0x00 LOAD_LOW  0x04 LOAD_HIGH  0x08 CTRL  0x0c INT_CLR
  //   0x10 INT_RAW   0x20 LOCK (0xe551 unlocks)
  // One tick is 1/32768 s, so 300 s is 0x960000: LOAD_HIGH 0x96, LOAD_LOW 0.
  // 300 s and not 60 s because the MMU-off console trace needs far more than
  // 60 s to walk the whole memory map; the watchdog stays a bounded dead man's
  //  switch and is deliberately never kicked from the console.
  // CTRL = NEW_VER_EN | INT_EN | CNT_EN | RST_EN.
  ldr   x0, =0x641e0000
  ldr   w1, =0xe551
  str   w1, [x0, 0x20]
  mov   w1, #0x9
  str   w1, [x0, 0x0c]
  mov   w1, #0x96
  str   w1, [x0, 0x04]
  str   wzr, [x0, 0x00]
  mov   w1, #0xf
  str   w1, [x0, 0x08]
  str   wzr, [x0, 0x20]
  dsb   sy

_EnterSec:
  FBSET 0xffff                  // white: watchdog armed, entering SEC

#ifdef SHIM_SELFTEST
  // Park here and blink, so that both facts are visible at once: the CPU is
  // alive (the colour changes) and the watchdog deadline is measurable (the
  // screen stops changing when the SoC resets).
  //
  // Bounded on purpose: after SHIM_BLINKS half second periods the shim falls
  // through into SEC anyway, so a boot that nobody is watching still ends in a
  // crash that LK turns into a reset instead of hanging forever.
  mov   x20, #SHIM_BLINKS
_Blink:
  FBSET 0xf800                  // red
  FBDELAY 400
  FBSET 0x07e0                  // green
  FBDELAY 400
  subs  x20, x20, 1
  b.ne  _Blink
#endif

  // Restore the FDT pointer and enter SEC.  x5 is reloaded because the blink
  // loop above uses it as a scratch register.
  mov   x0, x19
  ldr   x5, =FD_BASE
  br    x5

_Dead:
  wfe
  b     _Dead

.ltorg

//
//  Exception vectors.
//
//  Nothing in Silicium ever writes VBAR: SecVectorTable exists in
//  SiliciumPkg/Sec/AArch64/Exception.S, but no code loads it, so every
//  exception inside SEC lands on the vectors LK left behind and LK's panic
//  path resets the board with no trace.  These are installed by _Start above
//  and stay live from the first instruction of the shim until DXE core
//  installs its own, which covers the whole window we are chasing.
//
//  AArch64 requires VBAR[10:0] == 0, so the table is 2 KiB aligned, and it is
//  sixteen entries of exactly 0x80 bytes each, in this order:
//
//    0..3    Current EL with SP0      sync irq fiq serr
//    4..7    Current EL with SPx      sync irq fiq serr
//    8..11   Lower EL using AArch64   sync irq fiq serr
//    12..15  Lower EL using AArch32   sync irq fiq serr
//
//  SEC runs on its own stack, so group 1 is where a fault there lands; group 0
//  covers the window in which SP is still the shim's.  The lower EL groups are
//  here for completeness.
//
//  Each entry only passes its vector index and branches to the collector, so
//  no entry can overflow its 0x80 bytes.
//
.balign 2048
.global _VectorBase
_VectorBase:
.set _idx, 0
.rept 16
  mov   x7, #_idx
  b     _VectorEntry
  .balign 0x80                  // pad this entry out to its full 0x80 bytes
  .set _idx, _idx + 1
.endr
_VectorEnd:
.balign 0x80                    // the collector lives past the table, not in it

//  Common collector.  Moves to the dedicated exception stack, then fills the
//  argument registers the C handler wants:
//
//    x0 = vector index   x1 = ELR   x2 = ESR   x3 = FAR
//    x4 = SPSR           x5 = SP    x6 = CurrentEL   x7 = entry count
//
//  ELR_EL2/ESR_EL2/FAR_EL2/SPSR_EL2 are undefined at EL1 and vice versa, so
//  the bank is chosen from CurrentEL.
_VectorEntry:
  mov   x0, x7                        // vector index
  mov   x5, sp                        // the faulting SP, before we switch
  adr   x8, _ExcStackTop
  mov   sp, x8

  // Escalation counter: a fault while reporting is reported again with a
  // higher stage, so that a panel write into an unmapped 0xbe000000 still
  // leaves a ramoops record behind instead of looping on the same fault.
  adr   x9, _ExcStage
  ldr   w7, [x9]
  add   w10, w7, #1
  str   w10, [x9]

  mrs   x6, currentel
  ubfx  x6, x6, #2, #2
  cmp   x6, #2
  b.ne  _VectorEl1
  mrs   x1, elr_el2
  mrs   x2, esr_el2
  mrs   x3, far_el2
  mrs   x4, spsr_el2
  b     _VectorCall
_VectorEl1:
  mrs   x1, elr_el1
  mrs   x2, esr_el1
  mrs   x3, far_el1
  mrs   x4, spsr_el1
_VectorCall:
  bl    ShimExceptionReport
_VectorHang:
  wfe
  b     _VectorHang

//  How many times the handler has been entered.  0 on the first entry, so the
//  handler knows whether it can still trust its own memory writes.
.balign 4
_ExcStage:
  .word 0

//  4 KiB of handler stack.  PROGBITS and inside .shimstart on purpose:
//  llvm-objcopy -O binary does not emit NOBITS (.bss), so a .bss stack would not
//  be part of the image and the copy loop would load whatever the linker left.
.balign 16
_ExcStack:
  .space 4096, 0
_ExcStackTop:

//  End of image marker.  BootShim.ld places this after the C object, and it is
//  deliberately real data rather than alignment padding: llvm-objcopy sizes the
//  output by the end of the last section that has contents, so 16 bytes of
//  marker make the image end on an exact 16 byte boundary instead of on padding
//  that objcopy would drop.  That is what makes _Payload's file offset equal the
//  shim length, which is what LK relies on when it puts the FV right behind us.
.section .shimtail, "a", %progbits
.balign 16
_ShimEndMark:
  .ascii "SHIM-END-MARK-16"     // exactly 16 bytes

//  _Payload opens its own section so that it is the very last thing in the image
//  even though the C object is linked before it.  It has no contents, so the
//  image ends exactly here.
.section .shimend, "a", %progbits
.balign 16
.global _Payload
_Payload:

