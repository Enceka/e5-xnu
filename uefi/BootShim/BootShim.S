//
//  Boot shim for the Rongyue E5 (Unisoc UMS9621).
//
//  LK, the stock Android bootloader, always copies the boot partition kernel to
//  0x80080000 and jumps there with
//
//    x0 = physical address of the merged vendor FDT (0x86000000 on this board)
//    MMU off, caches off, EL2
//
//  EDK2 wants to live at PcdFdBaseAddress (0x80200000 for the E5), so this shim
//  sits at 0x80080000, copies the firmware volume that follows it up to
//  0x80200000, and branches to the FV base.  The FV zero vector there is a
//  branch to SEC, which picks up x0 and stashes it in the
//  gSiliciumEntryArgHobGuid HOB for DtPlatformDxe.
//
//  The first two words double as a valid AArch64 Image header, in case a loader
//  ever looks at them.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//

.set SHIM_BASE, 0x80080000

.section .text
.global _Head
.global _Start

.balign 4096
_Head:
  b     _Start                  // code0: branch to the real entry
  .long 0                       // code1: reserved
  .quad 0x80000                 // text_offset
  .quad 0                       // image_size, patched by build-e5.sh
  .quad 0                       // flags
  .quad 0                       // reserved
  .quad 0                       // reserved
  .quad 0                       // reserved
  .ascii "ARM"                  // magic, low half
  .byte 0x64                    // magic, high byte: "ARMd"
  .long 0                       // reserved

_Start:
  // Save the FDT pointer; the copy loop below clobbers x1..x10.
  mov   x19, x0

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

  // Copy the firmware volume to FD_BASE.
  mov   x7, x5
  mov   x8, x4
_CopyLoop:
  ldp   x2, x3, [x8], 16
  stp   x2, x3, [x7], 16
  subs  x6, x6, 16
  b.ne  _CopyLoop

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

  // Arm the AP watchdog as a dead man's switch before anything risky runs.
  //
  // Without it a hang leaves the board dead until the battery is pulled: LK's
  // panic handler (which does reset the SoC) only runs if we take an exception
  // into its EL2 vectors, and a wedged loop never does.  Measured the hard way.
  //
  // The AON register block is clock gated out of reset, and touching a gated
  // register wedges the AXI bus instead of faulting, so the two APB clocks have
  // to come up first.  Both are "set" gates in the aonapb controller at
  // 0x64900000 (syscon@64900000, aonapb-gate, "sprd,ums9621-aon-gate"): write
  // the mask to base + reg + 0x1000 to enable.
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
  //   0x20 LOCK (0xe551 unlocks)
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
  // Restore the FDT pointer and enter SEC.
  mov   x0, x19
  br    x5

_Dead:
  wfe
  b     _Dead

.ltorg

.balign 16
_Payload:
