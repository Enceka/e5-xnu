/** @file
  E5 bring-up probe.

  Everything in this firmware is being brought up blind: there is no serial
  console on the board we can attach to (UART1 has no accessible pads) and no
  USB stack in the FD, so the only channel back to a running Android is the
  ramoops console zone, which can only be read after LK rolls the slot back.

  That makes "the firmware silently wedged" the most expensive failure mode we
  have -- it costs a physical power cycle.  So instead of dropping into BDS and
  waiting forever for a key press, this driver runs at the end of DXE dispatch,
  prints a summary of what DXE discovered, and resets the machine.  LK then
  burns a boot attempt, rolls back to Android, and the ramoops log is readable
  within a couple of minutes.

  Copyright (c) 2026, e5-xnu contributors. SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>

#define E5_MAP_BUFFER_SIZE  (32 * 1024)

STATIC CONST CHAR8 *mMemoryTypeName (IN EFI_MEMORY_TYPE Type)
{
  switch (Type) {
  case EfiReservedMemoryType:      return "Reserved";
  case EfiLoaderCode:              return "LoaderCode";
  case EfiLoaderData:              return "LoaderData";
  case EfiBootServicesCode:        return "BsCode";
  case EfiBootServicesData:        return "BsData";
  case EfiRuntimeServicesCode:     return "RtCode";
  case EfiRuntimeServicesData:     return "RtData";
  case EfiConventionalMemory:      return "Conventional";
  case EfiUnusableMemory:          return "Unusable";
  case EfiACPIReclaimMemory:       return "ACPIReclaim";
  case EfiACPIMemoryNVS:           return "ACPINVS";
  case EfiMemoryMappedIO:          return "MMIO";
  case EfiMemoryMappedIOPortSpace: return "MMIOPort";
  case EfiPalCode:                 return "PalCode";
  case EfiPersistentMemory:        return "Persistent";
  default:                         return "Other";
  }
}

STATIC VOID E5DumpMemoryMap (VOID)
{
  EFI_STATUS             Status;
  EFI_MEMORY_DESCRIPTOR  *Map;
  UINTN                  MapSize;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Index;
  UINTN                  Count;
  UINTN                  Conventional;
  UINT64                 ConventionalPages;
  EFI_MEMORY_DESCRIPTOR  *Entry;

  MapSize = E5_MAP_BUFFER_SIZE;
  Map     = AllocatePool (MapSize);
  if (Map == NULL) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: out of pool for the memory map\n"));
    return;
  }

  Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: GetMemoryMap failed: %r\n", Status));
    FreePool (Map);
    return;
  }

  Count            = MapSize / DescriptorSize;
  Conventional     = 0;
  ConventionalPages = 0;
  for (Index = 0; Index < Count; Index++) {
    Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Index * DescriptorSize);
    if (Entry->Type == EfiConventionalMemory) {
      Conventional++;
      ConventionalPages += Entry->NumberOfPages;
    }
    DEBUG ((
      DEBUG_INFO,
      "E5-BRINGUP: map[%02u] %a 0x%016lx-0x%016lx attr 0x%lx\n",
      (UINT32)Index,
      mMemoryTypeName (Entry->Type),
      (UINT64)Entry->PhysicalStart,
      (UINT64)(Entry->PhysicalStart + EFI_PAGES_TO_SIZE (Entry->NumberOfPages)),
      (UINT64)Entry->Attribute
      ));
  }

  DEBUG ((
    DEBUG_INFO,
    "E5-BRINGUP: %u descriptors, %u conventional, %lu MiB free\n",
    (UINT32)Count,
    (UINT32)Conventional,
    (UINT64)EFI_PAGES_TO_SIZE (ConventionalPages) / (1024 * 1024)
    ));

  FreePool (Map);
}

STATIC VOID E5DumpGraphics (VOID)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "E5-BRINGUP: no GOP: %r\n", Status));
    return;
  }

  Info = Gop->Mode->Info;
  DEBUG ((
    DEBUG_INFO,
    "E5-BRINGUP: GOP mode %u/%u %ux%u %u bpp, frame buffer 0x%lx + 0x%lx\n",
    (UINT32)Gop->Mode->Mode,
    (UINT32)Gop->Mode->MaxMode,
    Info->HorizontalResolution,
    Info->VerticalResolution,
    Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ? 32 :
    Info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor ? 32 :
    Info->PixelFormat == PixelBitMask ? 16 : 0,
    (UINT64)Gop->Mode->FrameBufferBase,
    (UINT64)Gop->Mode->FrameBufferSize
    ));
}

STATIC VOID E5ResetToLoader (VOID)
{
  DEBUG ((DEBUG_INFO, "E5-BRINGUP: resetting to hand control back to LK\n"));
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  //
  // ResetSystem is wired to ArmPsciResetSystemLib (PSCI SYSTEM_RESET).  If the
  // SMC returns instead of resetting the SoC, park here and let the AP watchdog
  // that BootShim armed fire -- that is the whole reason it is armed.
  //
  DEBUG ((DEBUG_ERROR, "E5-BRINGUP: ResetSystem returned, parking for the watchdog\n"));
  CpuDeadLoop ();
}

STATIC VOID EFIAPI E5EndOfDxe (IN EFI_EVENT Event, IN VOID *Context)
{
  DEBUG ((DEBUG_INFO, "\nE5-BRINGUP: ===== end of DXE dispatch reached =====\n"));
  DEBUG ((
    DEBUG_INFO,
    "E5-BRINGUP: %u configuration table entries\n",
    (UINT32)gST->NumberOfTableEntries
    ));

  E5DumpGraphics ();
  E5DumpMemoryMap ();

  DEBUG ((DEBUG_INFO, "E5-BRINGUP: ===== end of DXE dump complete =====\n"));

  if (PcdGetBool (PcdE5ResetAtEndOfDxe)) {
    E5ResetToLoader ();
  }

  DEBUG ((DEBUG_INFO, "E5-BRINGUP: continuing into BDS (PcdE5ResetAtEndOfDxe is FALSE)\n"));
}

EFI_STATUS EFIAPI E5BringupInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  E5EndOfDxe,
                  NULL,
                  &gEfiEndOfDxeEventGroupGuid,
                  &Event
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: cannot hook end of DXE: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "E5-BRINGUP: armed, waiting for the end of DXE dispatch\n"));
  return EFI_SUCCESS;
}
