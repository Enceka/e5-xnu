##
#  APRIORI DXE dependency list for the E5.
#
#  Everything listed here is generic EDK2 or SiliciumPkg.  There is no vendor
#  blob and no Unisoc specific driver in the APRIORI set.
#
#  The storage stack is deliberately absent: the E5 eMMC controller has no EDK2
#  host driver yet, so SdDxe/EmmcDxe would block forever on a controller that
#  never shows up.  That comes with M5.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##
APRIORI DXE {
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf

  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf

  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf

!if $(USE_CUSTOM_DISPLAY_DRIVER) == 1
  INF UMS9621Pkg/Drivers/SprdGopDxe/SprdGopDxe.inf
!else
  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
!endif

  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  #
  # The graphics console driver is deliberately absent from this list.  The E5
  # panel is 320x480 and the font cell is 8x19, so MaxColumns = 320/8 = 40,
  # while the driver asserts (MaxColumns >= 80) && (MaxRows >= 25) at line 245
  # and then dead-loops into the AP watchdog (300s) -- the UEFI spec minimum of
  # 80 columns can never be met on this panel.  The serial console
  # (SimpleTextInOutSerial, above) carries the log instead.  Restore this entry
  # once a 40-column-capable graphics console driver exists.
  #
}
