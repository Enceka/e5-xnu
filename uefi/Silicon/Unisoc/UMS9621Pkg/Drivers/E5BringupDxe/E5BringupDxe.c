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
#include <Library/DevicePathLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Pi/PiFirmwareFile.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>

#define E5_MAP_BUFFER_SIZE  (32 * 1024)

//
// FILE_GUID of ShellPkg/Application/Shell/Shell.inf (gUefiShellFileGuid), the
// same value e5.dsc puts in PcdBootManagerMenuFile.  EFI_GUID's first three
// fields are integers, so they are written as the GUID's own values here; the
// byte-swapped form in the DSC is only the PCD's wire encoding.
//
STATIC EFI_GUID  mShellFileGuid = {
  0x7C04A583, 0x9E3E, 0x4f1c, { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }
};

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
  DEBUG ((DEBUG_ERROR, "E5-BRINGUP: resetting to hand control back to LK\n"));
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);

  //
  // ResetSystem is wired to ArmPsciResetSystemLib (PSCI SYSTEM_RESET).  If the
  // SMC returns instead of resetting the SoC, park here and let the AP watchdog
  // that BootShim armed fire -- that is the whole reason it is armed.
  //
  DEBUG ((DEBUG_ERROR, "E5-BRINGUP: ResetSystem returned, parking for the watchdog\n"));
  CpuDeadLoop ();
}

//
// Read the Shell's PE32 section out of whichever FV holds it.
//
// ReadSection with *Buffer == NULL makes the callee allocate, so the returned
// buffer is exactly the PE image and is ours to free.
//
STATIC EFI_STATUS E5ReadShellFromFv (
  OUT EFI_DEVICE_PATH_PROTOCOL  **ShellPath,
  OUT VOID                      **ShellImage,
  OUT UINTN                     *ShellImageSize
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     *Handles;
  UINTN                          HandleCount;
  UINTN                          Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
  VOID                           *Buffer;
  UINTN                          BufferSize;
  UINT32                         AuthenticationStatus;
  EFI_DEVICE_PATH_PROTOCOL       *FvDevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;

  *ShellPath      = NULL;
  *ShellImage     = NULL;
  *ShellImageSize = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: no FV handles: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "E5-BRINGUP: searching %u firmware volumes for the Shell\n", (UINT32)HandleCount));

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) {
      continue;
    }

    Buffer                = NULL;
    BufferSize            = 0;
    AuthenticationStatus  = 0;
    Status                = Fv->ReadSection (
                                  Fv,
                                  &mShellFileGuid,
                                  EFI_SECTION_PE32,
                                  0,
                                  &Buffer,
                                  &BufferSize,
                                  &AuthenticationStatus
                                  );
    if (EFI_ERROR (Status)) {
      continue;
    }

    FvDevicePath = DevicePathFromHandle (Handles[Index]);
    if (FvDevicePath == NULL) {
      //
      // LoadImage derives LoadedImage->DeviceHandle from this path, and the
      // Shell dereferences that handle unconditionally (Shell.c,
      // GetDevicePathsForImageAndFile -> OpenProtocol on DeviceHandle) under an
      // ASSERT_EFI_ERROR, so a handle without a path would dead-loop the board
      // instead of failing.  Skip it and keep looking.
      //
      DEBUG ((DEBUG_WARN, "E5-BRINGUP: FV %u holds the Shell but has no device path\n", (UINT32)Index));
      FreePool (Buffer);
      Status = EFI_NOT_FOUND;
      continue;
    }

    ZeroMem (&FileNode, sizeof (FileNode));
    FileNode.Header.Type    = MEDIA_DEVICE_PATH;
    FileNode.Header.SubType = MEDIA_PIWG_FW_FILE_DP;
    SetDevicePathNodeLength (&FileNode.Header, sizeof (FileNode));
    CopyGuid (&FileNode.FvFileName, &mShellFileGuid);

    *ShellPath = AppendDevicePathNode (FvDevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
    if (*ShellPath == NULL) {
      DEBUG ((DEBUG_ERROR, "E5-BRINGUP: cannot build the Shell device path\n"));
      FreePool (Buffer);
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    *ShellImage     = Buffer;
    *ShellImageSize = BufferSize;
    DEBUG ((
      DEBUG_INFO,
      "E5-BRINGUP: Shell found in FV %u, PE32 %u bytes, auth status 0x%x\n",
      (UINT32)Index,
      (UINT32)BufferSize,
      AuthenticationStatus
      ));
    break;
  }

  FreePool (Handles);
  return Status;
}

STATIC VOID E5LaunchShell (VOID)
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *ShellPath;
  VOID                       *ShellImage;
  UINTN                      ShellImageSize;
  EFI_HANDLE                 ShellHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  VOID                       *EntryPoint;
  UINTN                      ExitDataSize;
  CHAR16                     *ExitData;

  Status = E5ReadShellFromFv (&ShellPath, &ShellImage, &ShellImageSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: cannot read the Shell out of any FV: %r\n", Status));
    return;
  }

  //
  // Both the device path and the buffer are handed to LoadImage on purpose.
  // The buffer means DxeCore does not re-read the FV; the path is what gives
  // the new image a DeviceHandle and a FilePath, which the Shell needs (see
  // E5ReadShellFromFv) and which also lets the Security2 stub recognise the
  // image as coming from an FV and skip 3rd-party deferral.
  //
  ShellHandle = NULL;
  Status      = gBS->LoadImage (
                       FALSE,
                       gImageHandle,
                       ShellPath,
                       ShellImage,
                       ShellImageSize,
                       &ShellHandle
                       );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "E5-BRINGUP: LoadImage(Shell) failed: %r\n", Status));
    FreePool (ShellImage);
    FreePool (ShellPath);
    return;
  }

  Status = gBS->HandleProtocol (ShellHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (!EFI_ERROR (Status)) {
    EntryPoint = NULL;
    PeCoffLoaderGetEntryPoint (LoadedImage->ImageBase, &EntryPoint);
    DEBUG ((
      DEBUG_INFO,
      "E5-BRINGUP: Shell loaded, image handle %p, image base %p, entry point %p\n",
      ShellHandle,
      LoadedImage->ImageBase,
      EntryPoint
      ));
  } else {
    DEBUG ((DEBUG_WARN, "E5-BRINGUP: Shell loaded as %p, no LoadedImage: %r\n", ShellHandle, Status));
  }

  //
  // EndOfDxe notifications run at TPL_CALLBACK, which is a legal level to
  // StartImage from: CoreStartImage records the current TPL as the image's own
  // and asserts the same value on return, so the Shell simply runs at the TPL
  // it was started at.  Lowering TPL here would re-dispatch the EndOfDxe group
  // handlers that have not run yet, so it is deliberately left alone.
  //
  ExitDataSize = 0;
  ExitData     = NULL;
  Status       = gBS->StartImage (ShellHandle, &ExitDataSize, &ExitData);

  DEBUG ((
    DEBUG_ERROR,
    "E5-BRINGUP: Shell returned: %r, exit data size %u\n",
    Status,
    (UINT32)ExitDataSize
    ));

  FreePool (ShellImage);
  FreePool (ShellPath);
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

  //
  // Boot the Shell from here instead of letting BDS pick a boot option.
  // BDS refuses to auto-start the Shell because the boot option it builds from
  // PcdBootManagerMenuFile carries LOAD_OPTION_HIDDEN, so the only way to reach
  // the Shell is to start it directly.  Whatever happens above, execution
  // falls through to E5ResetToLoader() so the ramoops console is always
  // readable; nothing here is allowed to spin.
  //
  E5LaunchShell ();

  DEBUG ((DEBUG_ERROR, "E5-BRINGUP: Shell launch finished, resetting to collect the log\n"));
  E5ResetToLoader ();
}

EFI_STATUS EFIAPI E5BringupInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  DEBUG ((DEBUG_ERROR, "E5-BRINGUP: ENTRY REACHED\n"));

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
