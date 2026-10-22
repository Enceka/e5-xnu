/*
#
  E5 (Unisoc UMS9621 / ums9158_1h10) memory map.

  Every address below comes from one of two sources:
   - the live vendor FDT handed over by LK (dumps/live-fdt.dts, reserved-memory
     nodes plus the /soc ranges), or
   - the M0 bare-metal probe report (docs/m0-bringup-report.md).

  Layout rationale (LK hands control to us at 0x80080000 with MMU off):
   0x80080000  LK jumps here with x0 = vendor FDT.  Our BootShim sits here and
               copies the 4 MiB firmware volume that follows it up to
               0x80800000.  FD_BASE has to sit above the whole LK-loaded blob
               (0x80080000 + 4 MiB) or the forward copy would overwrite source
               bytes it has not read yet.
   0x80480000  primary core stack (0x40000, room for 8 of them).
   0x80680000  SEC heap.
   0x80780000  MMU page tables.
   0x80800000  UEFI FD, 4 MiB, this is where SEC really runs.
   0x80C00000  DXE heap, 84 MiB, ending where the FDT starts.
   0x86000000  vendor FDT passed in x0 by LK (285 820 bytes, reserve 256 KiB).
   0xBE000000  display / logo buffer, still being scanned out by the DPU.
   0xFFF80000  ramoops (AP watchdog dump + our debug log).

  The AON watchdog (0x641E0000) is deliberately NOT mapped: on this SoC its
  clock is gated until the AON block is enabled, and touching a gated register
  wedges the AXI bus instead of raising a bus fault (measured, see the M0
  report).  Leaving it unmapped turns an accidental access into a normal
  translation fault we can actually debug.
#

*/

#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // The blob LK copied into DRAM: our shim plus the firmware volume it moves up.
  {"Boot Shim",          0x80080000, 0x00400000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // DDR: firmware owned
  {"UEFI Stack",         0x80480000, 0x00200000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC Heap",           0x80680000, 0x00100000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"MMU PageTables",     0x80780000, 0x00040000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI FD",            0x80800000, 0x00400000, AddMem,  SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0x80C00000, 0x05400000, AddMem,  SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // DDR: reserved by the vendor firmware / secure world / modem
  {"DTB",                0x86000000, 0x00040000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, WRITE_BACK_XN},
  {"DDR Reserved",       0x87800000, 0x00800000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"CP/CMA Memory",      0x88000000, 0x1D000000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"AE6 Reserved",       0xAE600000, 0x00300000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"AF7 Reserved",       0xAF700000, 0x00300000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"AFA Reserved",       0xAFA00000, 0x00600000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"B000 Reserved",      0xB0000000, 0x00040000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"B006 Reserved",      0xB0060000, 0x03FA0000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"B400 Reserved",      0xB4000000, 0x00100000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display Reserved",   0xBE000000, 0x009E4000, AddMem,  MEM_RES, SYS_MEM_CAP,   Reserv, WRITE_THROUGH_XN},
  {"FFEF Reserved",      0xFFEF0000, 0x00080000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"FFF7 Reserved",      0xFFF70000, 0x00010000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},
  {"Ramoops",            0xFFF80000, 0x00040000, AddMem,  MEM_RES, UNCACHEABLE,   Reserv, UNCACHED_UNBUFFERED_XN},

  // On-chip peripherals
  {"GICD",               0x12000000, 0x00020000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"GICR",               0x12040000, 0x00100000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"UART0-3",            0x20200000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"EMMC",               0x22200000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"SDIO1",              0x22210000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"SDIO2 WIFI",         0x22220000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"SDIO3",              0x22230000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"I2C 0-6",            0x22240000, 0x00070000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DPU GLB",            0x30120000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DPU",                0x31000000, 0x00003000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"GSP",                0x31100000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DSI",                0x31300000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"DMAC",               0x60050000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"USB",                0x64000000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"AON PWM",            0x64030000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"EFUSE",              0x64160000, 0x00001000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PINCTRL",            0x64170000, 0x00010000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"GPIO",               0x64200000, 0x00070000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"THERMAL",            0x64270000, 0x00040000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE},
  {"PMIC ADI",           0x64400000, 0x00100000, AddDev,  MMAP_IO, UNCACHEABLE,   MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
