/** @file
  Register base addresses for the Unisoc UMS9621 (Qogirn6lite) as measured on
  the Rongyue E5.

  Everything here comes from the vendor FDT the stock LK passes to the kernel
  (dumps/live-fdt.dts in this repository) or from a register dump taken on the
  running device by the M0 bare metal probe (docs/m0-bringup-report.md).  Where
  the two disagree the measurement wins, and the comment says so.

  Copyright (c) 2026, e5-xnu contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UMS9621_H_
#define UMS9621_H_

//
// Interrupt controller: GICv3, 288 SPIs, 8 redistributor frames with a stride
// of 0x20000.  Note that GICR_TYPER.Last reads back as 0 on every frame, so the
// frame count cannot be derived from the hardware and has to come from the FDT
// (#redistributor-regions = 1) plus the CPU count.
//
#define UMS9621_GICD_BASE   0x12000000
#define UMS9621_GICD_SIZE   0x00020000
#define UMS9621_GICR_BASE   0x12040000
#define UMS9621_GICR_SIZE   0x00100000

//
// Arch timer: 26 MHz, PPIs 13/14/11/10 (secure-phy, non-secure-phy, virtual,
// hyp).  The ArmPkg defaults are 29/30/27/26 and must be overridden.
//
#define UMS9621_TIMER_FREQ  26000000

//
// Debug UART: sprd_serial, TXD at +0x00, LSR at +0x08, TX-over bit 15.
// UART1 is the one the stock firmware puts console= on.
//
#define UMS9621_UART0_BASE  0x20200000
#define UMS9621_UART1_BASE  0x20210000
#define UMS9621_UART_STRIDE 0x00010000

//
// Storage: eMMC on SDHCI at 0x22200000 (no UFS on this board).
//
#define UMS9621_EMMC_BASE   0x22200000

//
// Display: DPU 0x31000000, GLB 0x30120000, GSP 0x31100000, DSI 0x31300000.
// The bootloader leaves a frame buffer at 0xbe000000 (logobuffer, 0x9e4000
// bytes) configured for a 320x480 RGB565 panel with a 640 byte row pitch.
//
#define UMS9621_DPU_BASE    0x31000000
#define UMS9621_DPU_GLB     0x30120000
#define UMS9621_GSP_BASE    0x31100000
#define UMS9621_DSI_BASE    0x31300000

//
// Always-on block.  Everything in here is clock gated out of reset, and a
// read of a gated register wedges the AXI bus instead of faulting, so the gates
// have to be opened first.  The aonapb gate controller lives at 0x64900000 and
// takes "write the mask to base + reg + 0x1000" to enable.
//
#define UMS9621_AON_BASE          0x64000000
#define UMS9621_AONAPB_GATE_BASE  0x64900000
#define UMS9621_AONAPB_GATE_SET   0x00001000
#define UMS9621_AP_WDT_EB_REG     0x00000008
#define UMS9621_AP_WDT_EB_BIT     29
#define UMS9621_AP_WDT_RTC_EB_REG 0x00000018
#define UMS9621_AP_WDT_RTC_EB_BIT 7

//
// AP watchdog (sprd,wdt-r2p0-fiq).  One tick is 1/32768 s.
//
#define UMS9621_AP_WDT_BASE  0x641E0000
#define UMS9621_WDT_LOAD_LOW  0x00
#define UMS9621_WDT_LOAD_HIGH 0x04
#define UMS9621_WDT_CTRL      0x08
#define UMS9621_WDT_INT_CLR   0x0C
#define UMS9621_WDT_INT_RAW   0x10
#define UMS9621_WDT_LOCK      0x20
#define UMS9621_WDT_UNLOCK_KEY 0xE551
#define UMS9621_WDT_CTRL_INT_EN     0x1
#define UMS9621_WDT_CTRL_CNT_EN     0x2
#define UMS9621_WDT_CTRL_NEW_VER_EN 0x4
#define UMS9621_WDT_CTRL_RST_EN     0x8

//
// Ramoops: the only debug channel that survives into Android.  The console
// record starts with a 12 byte header (signature, start offset, length).
//
#define UMS9621_RAMOOPS_BASE        0xFFF80000
#define UMS9621_RAMOOPS_SIZE        0x00040000
#define UMS9621_RAMOOPS_CONSOLE     0xFFFB0000
#define UMS9621_RAMOOPS_CONSOLE_SIG 0x43474244

#endif /* UMS9621_H_ */
