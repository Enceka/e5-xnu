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

.section .text
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
  // One tick is 1/32768 s, so 60 s is 0x1e0000: LOAD_HIGH 0x1e, LOAD_LOW 0.
  // CTRL = NEW_VER_EN | INT_EN | CNT_EN | RST_EN.
  ldr   x0, =0x641e0000
  ldr   w1, =0xe551
  str   w1, [x0, 0x20]
  mov   w1, #0x9
  str   w1, [x0, 0x0c]
  mov   w1, #0x1e
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

.balign 16
_Payload:
