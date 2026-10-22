/** @file
  GOP driver for the Unisoc UMS9621 (Qogirn6lite) + ST7365P panel on the E5.

  SiliciumPkg SimpleFbDxe refuses to publish anything smaller than 640x475
  ("Frame Buffer Resolution is too Small"), and the E5 panel is 320x480, so
  this driver replaces it.  Geometry is fixed by PCD and there is exactly one
  mode: the native one.  There is no mode switching at all, because the DPU
  keeps scanning the same buffer the whole time, so SetMode only validates
  the mode number.

  The frame buffer address is taken from the "Display Reserved" entry of
  MemoryMapLib, which is the same region SiliciumPkg/Sec/Sec.c clears before
  DXE is entered.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FrameBufferBltLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Configuration/BootDevices.h>
#include <Protocol/GraphicsOutput.h>

//
// Names tried when looking up the reserved frame buffer region.  Both
// spellings exist in the wild, so both are attempted.
//
STATIC CHAR8 *mFrameBufferRegionNames[] = {
  "Display Reserved",
  "Display_Reserved"
};

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL         mSprdGop;
STATIC FRAME_BUFFER_CONFIGURE               *mFrameBufferConfiguration;
STATIC EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  mModeInfo;

/**
  Return the single mode this panel supports.

  @param[in]  This        A pointer to the EFI_GRAPHICS_OUTPUT_PROTOCOL instance.
  @param[in]  ModeNumber  Mode number, must be 0.
  @param[out] SizeOfInfo  Size in bytes of the returned structure.
  @param[out] Info        Caller allocated buffer describing the mode.

  @retval EFI_SUCCESS      The mode was returned.
  @retval EFI_DEVICE_ERROR ModeNumber is out of range.
**/
STATIC
EFI_STATUS
EFIAPI
SprdGopQueryMode (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
  IN  UINT32                                 ModeNumber,
  OUT UINTN                                 *SizeOfInfo,
  OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info)
{
  EFI_STATUS Status;

  if (ModeNumber >= This->Mode->MaxMode) {
    return EFI_DEVICE_ERROR;
  }

  Status = gBS->AllocatePool (EfiBootServicesData, sizeof (mModeInfo), (VOID **)Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CopyMem (*Info, &mModeInfo, sizeof (mModeInfo));
  *SizeOfInfo = sizeof (mModeInfo);

  return EFI_SUCCESS;
}

/**
  Select a mode.  There is only one, and the DPU never stops scanning it, so
  this is a pure validation.

  @param[in] This        A pointer to the EFI_GRAPHICS_OUTPUT_PROTOCOL instance.
  @param[in] ModeNumber  Mode number, must be 0.

  @retval EFI_SUCCESS      The mode was selected.
  @retval EFI_DEVICE_ERROR ModeNumber is out of range.
**/
STATIC
EFI_STATUS
EFIAPI
SprdGopSetMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
  IN UINT32                        ModeNumber)
{
  if (ModeNumber >= This->Mode->MaxMode) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Blit a rectangle, optionally converting from a BLT pixel buffer.

  @param[in] This         A pointer to the EFI_GRAPHICS_OUTPUT_PROTOCOL instance.
  @param[in] BltBuffer    Source or destination buffer, see BltOperation.
  @param[in] BltOperation The operation to perform.
  @param[in] SourceX      Source X coordinate.
  @param[in] SourceY      Source Y coordinate.
  @param[in] DestinationX Destination X coordinate.
  @param[in] DestinationY Destination Y coordinate.
  @param[in] Width        Width of the rectangle.
  @param[in] Height       Height of the rectangle.
  @param[in] Delta        Source buffer stride in pixels, 0 means Width.

  @retval EFI_SUCCESS The rectangle was blitted.
  @return              Errors from FrameBufferBlt.
**/
STATIC
EFI_STATUS
EFIAPI
SprdGopBlt (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL       *This,
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL      *BltBuffer,
  IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION   BltOperation,
  IN UINTN                               SourceX,
  IN UINTN                               SourceY,
  IN UINTN                               DestinationX,
  IN UINTN                               DestinationY,
  IN UINTN                               Width,
  IN UINTN                               Height,
  IN UINTN                               Delta)
{
  EFI_STATUS Status;
  EFI_TPL    Tpl;

  Tpl    = gBS->RaiseTPL (TPL_NOTIFY);
  Status = FrameBufferBlt (
             mFrameBufferConfiguration,
             BltBuffer,
             BltOperation,
             SourceX,
             SourceY,
             DestinationX,
             DestinationY,
             Width,
             Height,
             Delta
             );
  gBS->RestoreTPL (Tpl);

  return Status;
}

/**
  Look up the reserved frame buffer region.

  @param[out] BaseAddress Base address of the frame buffer.

  @retval EFI_SUCCESS          The region was found.
  @retval EFI_NOT_FOUND        No region with a known name exists.
  @retval EFI_BUFFER_TOO_SMALL The region is smaller than the mode needs.
**/
STATIC
EFI_STATUS
SprdGopFindFrameBuffer (
  OUT EFI_PHYSICAL_ADDRESS *BaseAddress)
{
  EFI_MEMORY_REGION_DESCRIPTOR Region;
  UINTN                        Index;
  UINTN                        Needed;

  ZeroMem (&Region, sizeof (Region));

  for (Index = 0; Index < ARRAY_SIZE (mFrameBufferRegionNames); Index++) {
    ZeroMem (&Region, sizeof (Region));
    LocateMemoryRegionByName (mFrameBufferRegionNames[Index], &Region);
    if (Region.Address != 0) {
      break;
    }
  }

  if (Region.Address == 0) {
    DEBUG ((DEBUG_ERROR, "SprdGop: no Display Reserved region in MemoryMapLib\n"));
    return EFI_NOT_FOUND;
  }

  Needed = (UINTN)FixedPcdGet32 (PcdFrameBufferWidth) *
           (UINTN)FixedPcdGet32 (PcdFrameBufferHeight) *
           ((UINTN)FixedPcdGet32 (PcdFrameBufferColorDepth) / 8);

  if (Region.Length < Needed) {
    DEBUG ((DEBUG_ERROR, "SprdGop: region 0x%lx has %lu bytes, need %lu\n", Region.Address, Region.Length, Needed));
    return EFI_BUFFER_TOO_SMALL;
  }

  *BaseAddress = Region.Address;

  return EFI_SUCCESS;
}

/**
  Fill in the single mode description and allocate the BLT configuration.

  @param[in] BaseAddress Frame buffer base address.

  @retval EFI_SUCCESS Everything is ready.
  @return             Errors from FrameBufferBltConfigure or allocation.
**/
STATIC
EFI_STATUS
SprdGopSetupMode (
  IN EFI_PHYSICAL_ADDRESS BaseAddress)
{
  EFI_STATUS Status;
  UINTN      ConfigurationSize;
  UINT32     Width;
  UINT32     Height;
  UINT32     ColorDepth;

  Width      = FixedPcdGet32 (PcdFrameBufferWidth);
  Height     = FixedPcdGet32 (PcdFrameBufferHeight);
  ColorDepth = FixedPcdGet32 (PcdFrameBufferColorDepth);

  ZeroMem (&mModeInfo, sizeof (mModeInfo));

  mModeInfo.Version              = 0;
  mModeInfo.HorizontalResolution = Width;
  mModeInfo.VerticalResolution   = Height;
  mModeInfo.PixelsPerScanLine    = Width;

  switch (ColorDepth) {
    case 32:
      mModeInfo.PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
      break;

    case 24:
      mModeInfo.PixelFormat                   = PixelBitMask;
      mModeInfo.PixelInformation.RedMask      = 0x00FF0000;
      mModeInfo.PixelInformation.GreenMask    = 0x0000FF00;
      mModeInfo.PixelInformation.BlueMask     = 0x000000FF;
      mModeInfo.PixelInformation.ReservedMask = 0x00000000;
      break;

    case 16:
      //
      // RGB565, which is what LK leaves programmed in DPU LAY0_CTRL on this
      // board: format field 4, pitch 320 pixels, stride 640 bytes.
      //
      mModeInfo.PixelFormat                   = PixelBitMask;
      mModeInfo.PixelInformation.RedMask      = 0x0000F800;
      mModeInfo.PixelInformation.GreenMask    = 0x000007E0;
      mModeInfo.PixelInformation.BlueMask     = 0x0000001F;
      mModeInfo.PixelInformation.ReservedMask = 0x00000000;
      break;

    default:
      DEBUG ((DEBUG_ERROR, "SprdGop: unsupported color depth %u\n", ColorDepth));
      return EFI_UNSUPPORTED;
  }

  mSprdGop.Mode->MaxMode         = 1;
  mSprdGop.Mode->Mode            = 0;
  mSprdGop.Mode->Info            = &mModeInfo;
  mSprdGop.Mode->SizeOfInfo      = sizeof (mModeInfo);
  mSprdGop.Mode->FrameBufferBase = BaseAddress;
  mSprdGop.Mode->FrameBufferSize = Width * Height * (ColorDepth / 8);

  ConfigurationSize = 0;
  Status            = FrameBufferBltConfigure (
                        (VOID *)(UINTN)BaseAddress,
                        &mModeInfo,
                        mFrameBufferConfiguration,
                        &ConfigurationSize
                        );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((DEBUG_ERROR, "SprdGop: FrameBufferBltConfigure sizing failed: %r\n", Status));
    return Status;
  }

  mFrameBufferConfiguration = AllocatePool (ConfigurationSize);
  if (mFrameBufferConfiguration == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = FrameBufferBltConfigure (
             (VOID *)(UINTN)BaseAddress,
             &mModeInfo,
             mFrameBufferConfiguration,
             &ConfigurationSize
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SprdGop: FrameBufferBltConfigure failed: %r\n", Status));
    FreePool (mFrameBufferConfiguration);
    mFrameBufferConfiguration = NULL;
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Driver entry point: publish a GOP for the 320x480 RGB565 panel.

  @param[in] ImageHandle The firmware allocated handle for the image.
  @param[in] SystemTable A pointer to the EFI system table.

  @retval EFI_SUCCESS The GOP was installed.
  @return             Errors from region lookup, setup or installation.
**/
EFI_STATUS
EFIAPI
SprdGopInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS BaseAddress;

  mSprdGop.QueryMode = SprdGopQueryMode;
  mSprdGop.SetMode   = SprdGopSetMode;
  mSprdGop.Blt       = SprdGopBlt;

  mSprdGop.Mode = AllocateZeroPool (sizeof (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE));
  if (mSprdGop.Mode == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = SprdGopFindFrameBuffer (&BaseAddress);
  if (EFI_ERROR (Status)) {
    goto Cleanup;
  }

  Status = SprdGopSetupMode (BaseAddress);
  if (EFI_ERROR (Status)) {
    goto Cleanup;
  }

  DEBUG ((
    DEBUG_INFO,
    "SprdGop: %ux%u %u bpp at 0x%lx, stride %u bytes\n",
    mModeInfo.HorizontalResolution,
    mModeInfo.VerticalResolution,
    FixedPcdGet32 (PcdFrameBufferColorDepth),
    BaseAddress,
    mModeInfo.PixelsPerScanLine * (FixedPcdGet32 (PcdFrameBufferColorDepth) / 8)
    ));

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDevicePathProtocolGuid,
                  &DisplayDevicePath,
                  &gEfiGraphicsOutputProtocolGuid,
                  &mSprdGop,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "SprdGop: InstallMultipleProtocolInterfaces failed: %r\n", Status));
  }

Cleanup:
  if (EFI_ERROR (Status)) {
    if (mFrameBufferConfiguration != NULL) {
      FreePool (mFrameBufferConfiguration);
      mFrameBufferConfiguration = NULL;
    }

    if (mSprdGop.Mode != NULL) {
      FreePool (mSprdGop.Mode);
      mSprdGop.Mode = NULL;
    }
  }

  return Status;
}
