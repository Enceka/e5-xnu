##
#  Unisoc UMS9621 (Qogirn6lite) SoC package
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

[Defines]
  #
  # The arch timer runs on the non-secure physical counter on this SoC.
  #
  USE_PHYSICAL_TIMER                         = 1

!include SiliciumPkg/SiliciumPkg.dsc.inc

[BuildOptions]
  #
  # SiliciumPkg only suppresses these warnings for CLANGPDB; we build with
  # CLANGDWARF (ELF + GenFw) so the same suppressions have to be repeated here.
  #
  *_CLANGDWARF_*_CC_FLAGS = -Wno-unused-const-variable -Wno-sometimes-uninitialized -Wno-deprecated-non-prototype -Wno-unaligned-access -fno-stack-protector
  *_CLANGDWARF_*_CC_FLAGS = -D CLANG -D ENABLE_SECUREBOOT=0

[PcdsFixedAtBuild]
  #
  # CPU: 6x Cortex-A55 + 2x Cortex-A76, all brought up through PSCI 0.2.
  #
  gArmPlatformTokenSpaceGuid.PcdCoreCount|8
  gArmPlatformTokenSpaceGuid.PcdClusterCount|2

  #
  # GICv3: GICD@0x12000000 (128 KiB), GICR@0x12040000 (1 MiB, stride 0x20000).
  # Measured on hardware: 8 redistributors, all with GICR_TYPER.Last = 0, so
  # the redistributor count must come from PcdCoreCount, never from Last.
  #
  gArmTokenSpaceGuid.PcdGicDistributorBase|0x12000000
  gArmTokenSpaceGuid.PcdGicRedistributorsBase|0x12040000

  #
  # Arch timer: this SoC does not use ARM's conventional 29/30/27/26 PPI
  # numbers.  The vendor FDT says
  #   interrupts = <1 13 4>, <1 14 4>, <1 11 4>, <1 10 4>
  # in secure-physical, non-secure-physical, virtual, hypervisor order, and
  # ArmPkg defaults to 29/30/27/26, so without these overrides TimerDxe would
  # arm a PPI that never fires and DXE would stall on the first timer event.
  #
  gArmTokenSpaceGuid.PcdArmArchTimerSecIntrNum|13
  gArmTokenSpaceGuid.PcdArmArchTimerIntrNum|14
  gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum|11
  gArmTokenSpaceGuid.PcdArmArchTimerHypIntrNum|10

  #
  # SMBIOS strings.
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorModel|"Unisoc UMS9621 (Qogirn6lite)"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosProcessorPartNumber|"UMS9621"

[LibraryClasses]
  #
  # Debug output goes to UART1 (0x20210000) and to the ramoops console zone
  # (0xfffb0000).  There is no exposed UART header on this board, so ramoops is
  # the primary channel: read it back from Android with
  #   adb exec-out cat /sys/fs/pstore/console-ramoops-0
  #
  SerialPortLib|UMS9621Pkg/Library/SprdDebugSerialPortLib/SprdDebugSerialPortLib.inf
