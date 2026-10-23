##
#  EDK2 platform description for the Rongyue E5 (Unisoc UMS9621 / Qogirn6lite).
#
#  The E5 has no UEFI firmware: LK (the stock Android bootloader) loads whatever
#  sits at offset 0x1000 of the boot partition to 0x80080000 and jumps to it
#  with x0 = physical address of the merged vendor FDT.  BootShim (see
#  ../../BootShim) turns that into a normal EDK2 entry: it copies the FD to
#  PcdFdBaseAddress and branches to SEC, keeping x0 intact so that SEC can
#  stash it in the gSiliciumEntryArgHobGuid HOB.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  PLATFORM_NAME                  = e5
  PLATFORM_GUID                  = 1C0A6E52-3D74-4F1B-8E29-5A7C0D4B93F8
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/e5Pkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = e5Pkg/e5.fdf
  #
  # The panel is 320x480, far below the 640x475 SimpleFbDxe insists on, so the
  # platform supplies its own GOP driver (SprdGopDxe).
  #
  USE_CUSTOM_DISPLAY_DRIVER      = 1

!include UMS9621Pkg/UMS9621Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR: 2 GiB at 0x80000000 (memory@80000000 in the vendor FDT).
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI stack: inside the "UEFI Stack" region of MemoryMapLib.
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x80480000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # 调试打印级别：0x8007EECF = ERROR(0x80000000) | INFO(0x40) | DISPATCH(0x80)
  #                            | 原有的 0x0007EE0F 其余位。
  #
  # 0x0F -> 0xCF：打开 bit6 DEBUG_INFO 与 bit7 DEBUG_DISPATCH。前者让各库的
  # INFO 级 trace 可见，后者让 DXE Core 在每次派发驱动时打印
  # "Loading driver at ... EntryPoint=..."，崩溃时可直接指名是哪个驱动。
  #
  # 同时清掉 bit25 DEBUG_PAGING(0x02000000)：该位原先是为定位 SEC 的 MMU 挂死点
  # 临时打开的（根因是 MemoryMapLib 里 UART0-3 长度写成 0x10000，控制台 UART1
  # 0x20210000 未被映射），问题已修复。DXE 阶段 MemoryProtection 会频繁调用
  # ArmSetMemoryAttributes，残留的 PAGING 输出会再次撑爆 32741 字节的 ramoops
  # 环形缓冲，把 SEC 阶段的日志挤掉，而我们需要 SEC+DXE 连续日志。
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8007EECF

  #
  # 调试属性掩码：0x2F -> 0x2B -> 0x23，依次清掉 bit2 DEBUG_CODE_ENABLE(0x04)
  # 与 bit3 CLEAR_MEMORY_ENABLED(0x08)。
  #
  # DEBUG_CODE_ENABLE 会把 DEBUG_CODE_BEGIN()/DEBUG_CODE_END() 之间的代码编进
  # 二进制（CpuExceptionHandlerLib 里是栈回溯 walker）。该 walker 在池指针被
  # 0xAF 填满的崩溃现场会二次异常，日志停在 "Recursive exception occurred while
  # dumping the CPU state"，DEBUG_CODE_END() 之后的栈 dump 永远执行不到。
  # 关掉 bit2 后整段回溯代码不编译，寄存器 dump 与栈 dump 均可正常输出。
  #
  # CLEAR_MEMORY_ENABLED 会让 FreePages 用 PcdDebugClearMemoryValue(0xAF) 填满
  # 刚释放的页，包括 ArmMmuLib 释放的页表页。0xAF 的低 2 位是 0x3，恰好等于
  # TT_TYPE_TABLE_ENTRY，于是父级表项仍指向的毒化页被 IsTableEntry() 当成合法
  # 页表继续遍历，解引用 0xAFAFAFAFAFAFA000 触发 Translation fault（FAR
  # 0xAFAFAFAFAFAFAFB0）。关掉 bit3 后释放页不再被毒化。
  # 保留的位：ASSERT_ENABLE(0x01) | PRINT_ERROR(0x02) | ASSERT_DEADLOOP(0x20)。
  #
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x23

  #
  # Boot Manager menu: BDS looks this GUID up in the FV and, if it is missing,
  # logs "BootManagerMenu FFS section can not be found" and registers no boot
  # option at all.  The value inherited from SiliciumPkg.dsc.inc points at a
  # file that this platform never puts in its FV, so override it with the UEFI
  # Shell's FILE_GUID (gUefiShellFileGuid, ShellPkg/Application/Shell/Shell.inf)
  # and BDS gets a boot target that actually exists.
  #
  # EFI_GUID byte layout: Data1/Data2/Data3 are scalars, so they land in memory
  # little-endian, while Data4 is an array of bytes and is copied verbatim.
  #   7C04A583 -> 83 A5 04 7C
  #   9E3E     -> 3E 9E
  #   4f1c     -> 1C 4F
  #   AD65-E05268D0B4D1 -> AD 65 E0 52 68 D0 B4 D1
  #
  gEfiMdeModulePkgTokenSpaceGuid.PcdBootManagerMenuFile|{ 0x83, 0xA5, 0x04, 0x7C, 0x3E, 0x9E, 0x1C, 0x4F, 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }

  #
  # Stop resetting at EndOfDxe now that BDS finally has a boot target.
  # With the default (TRUE) E5BringupDxe resets to LK right after its dump, so
  # the Shell never gets launched and the DXE-after path stays untested.
  #
  gUms9621PkgTokenSpaceGuid.PcdE5ResetAtEndOfDxe|FALSE

  #
  # SMBIOS.
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Rongyue"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"E5"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"e5"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"E5_ums9158_1h10"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"ums9158_1h10"

  #
  # Panel: 320x480, RGB565.  Measured from DPU PANEL_SIZE = 0x01e00140
  # (480 << 16 | 320) and LAY0_CTRL format field 4 on the running device.
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|320
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|480
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|16

[LibraryClasses]
  #
  # Memory map: describes every reserved region of the vendor FDT plus the
  # peripherals EDK2 needs.  Also the source of "Display Reserved", which both
  # SEC (to clear the panel) and SprdGopDxe (to publish GOP) look up by name.
  #
  MemoryMapLib|e5Pkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # UEFI Shell.  These three are the only ShellPkg libraries the include chain
  # does not already name; the rest of what Shell.inf and the command profiles
  # ask for (HiiLib, UefiHiiServicesLib, FileHandleLib, SortLib,
  # PeCoffGetEntryPointLib from SiliciumPkg.dsc.inc, OrderedCollectionLib from
  # MdeLibs.dsc.inc) is inherited and must not be repeated here.
  #
  ShellLib|ShellPkg/Library/UefiShellLib/UefiShellLib.inf
  ShellCommandLib|ShellPkg/Library/UefiShellCommandLib/UefiShellCommandLib.inf
  HandleParsingLib|ShellPkg/Library/UefiHandleParsingLib/UefiHandleParsingLib.inf

[Components]
  #
  # Display: our own GOP, because SimpleFbDxe rejects anything under 640x475.
  #
  UMS9621Pkg/Drivers/SprdGopDxe/SprdGopDxe.inf

  #
  # Bring-up probe: dumps the DXE view of the world at the end of dispatch and
  # resets, so that every experiment ends with a readable ramoops log instead of
  # a board that has to be power cycled by hand.
  #
  UMS9621Pkg/Drivers/E5BringupDxe/E5BringupDxe.inf

  #
  # Payload volume: publishes the boot image ramdisk (/chosen linux,initrd-start)
  # as a read-only block device so the FAT payload inside it becomes a UEFI
  # volume. Built here so it keeps compiling, but deliberately absent from
  # e5.fdf until the DXE core is confirmed running on hardware -- a fault
  # during dispatch would hide the EndOfDxe marker.
  #
  e5Pkg/Drivers/E5PayloadDxe/E5PayloadDxe.inf

  #
  # UEFI Shell: the boot target BDS was missing.  Without it BmBoot finds no
  # boot option, and PcdE5ResetAtEndOfDxe resets straight back to LK, so the
  # DXE-after path never gets to run a UEFI image.
  #
  # Only the Level1/2/3 command profiles are linked in.  Driver1/Install1/
  # Debug1/Network1/Network2 would each add another NULL library and drag in
  # NetLib, BcfgCommandLib and crypto, none of which this board can use yet;
  # add them one at a time if a missing command ever matters.
  #
  # PcdShellLibAutoInitialize|FALSE stops ShellLib from initialising in its
  # constructor and capturing the console -- Shell installs the Shell and
  # ShellParameters protocols itself, and the library must not get there first.
  #
  ShellPkg/Application/Shell/Shell.inf {
    <LibraryClasses>
      NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
      NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
    <PcdsFixedAtBuild>
      gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
  }
