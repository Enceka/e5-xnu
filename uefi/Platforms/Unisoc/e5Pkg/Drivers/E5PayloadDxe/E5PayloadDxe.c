/** @file
  Publish the Android boot image ramdisk as a read-only block device.

  LK places the boot image ramdisk in DRAM and records both ends in the device
  tree it hands over in x0 (/chosen linux,initrd-start and linux,initrd-end).
  The UniXNU payload is a FAT image inside that ramdisk, so publishing a block
  device over it is what turns the payload into a normal UEFI volume that
  DarwinBoot can read the kernel and its .adt from.

  Measured on the real device: LK always puts the ramdisk at 0xC0000000 and the
  Android vendor_boot ramdisk ends at 0xC3640644. The MemoryMapLib entry named
  "Initrd" reserves 0xC0000000..0xC8000000 so the MMU covers it; the exact size
  comes from the device tree at run time, never from this file.

  NOT WIRED IN YET: e5.dsc and e5.fdf gain this driver only after the DXE core
  is confirmed running, because a fault during dispatch would hide the
  EndOfDxe marker that E5BringupDxe prints.
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#define RAM_BLOCK_SIZE  512
#define DEBUG_TAG       "E5PAYLOAD: "

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} RAM_BLOCK_DEVICE_PATH;
#pragma pack()

STATIC RAM_BLOCK_DEVICE_PATH  mDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) } },
    { 0x5D3E8A17, 0x42C6, 0x4B0F, { 0x9D, 0x85, 0x6A, 0x1C, 0x7E, 0x93, 0xB2, 0x04 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC UINT8               *mRamBase;
STATIC EFI_BLOCK_IO_MEDIA   mMedia;

STATIC
UINT64
ReadCells (
  IN CONST VOID  *Data,
  IN UINT32      Cells
  )
{
  if (Cells == 2) {
    return Fdt64ToCpu (ReadUnaligned64 (Data));
  }

  return Fdt32ToCpu (ReadUnaligned32 (Data));
}

STATIC
EFI_STATUS
EFIAPI
RamBlockReset (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockRead (
  IN  EFI_BLOCK_IO_PROTOCOL  *This,
  IN  UINT32                 MediaId,
  IN  EFI_LBA                Lba,
  IN  UINTN                  BufferSize,
  OUT VOID                   *Buffer
  )
{
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MediaId != mMedia.MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((BufferSize % RAM_BLOCK_SIZE) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if ((Lba > mMedia.LastBlock) || ((BufferSize / RAM_BLOCK_SIZE) > (mMedia.LastBlock + 1 - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Buffer, mRamBase + Lba * RAM_BLOCK_SIZE, BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockWrite (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return EFI_WRITE_PROTECTED;
}

STATIC
EFI_STATUS
EFIAPI
RamBlockFlush (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_BLOCK_IO_PROTOCOL  mBlockIo = {
  EFI_BLOCK_IO_PROTOCOL_REVISION,
  &mMedia,
  RamBlockReset,
  RamBlockRead,
  RamBlockWrite,
  RamBlockFlush
};

STATIC
VOID
PublishInitrd (
  IN CONST VOID  *Fdt
  )
{
  EFI_STATUS   Status;
  INT32        Chosen;
  CONST VOID   *StartProp;
  CONST VOID   *EndProp;
  INT32        StartLength;
  INT32        EndLength;
  UINT64       Start;
  UINT64       End;
  EFI_HANDLE   Handle;

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen < 0) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "no /chosen in the handed over tree\n"));
    return;
  }

  StartProp = FdtGetProp (Fdt, Chosen, "linux,initrd-start", &StartLength);
  EndProp   = FdtGetProp (Fdt, Chosen, "linux,initrd-end", &EndLength);
  if ((StartProp == NULL) || (EndProp == NULL) ||
      ((StartLength != 4) && (StartLength != 8)) || ((EndLength != 4) && (EndLength != 8)))
  {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "no linux,initrd-start/-end; the boot image carried no ramdisk\n"));
    return;
  }

  Start = ReadCells (StartProp, (UINT32)(StartLength / 4));
  End   = ReadCells (EndProp, (UINT32)(EndLength / 4));
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd 0x%lx-0x%lx (%lu bytes)\n", Start, End, End - Start));

  if ((End <= Start) || (((End - Start) % RAM_BLOCK_SIZE) != 0)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd is not a whole number of 512-byte sectors\n"));
    return;
  }

  mRamBase                = (UINT8 *)(UINTN)Start;
  mMedia.MediaId          = 1;
  mMedia.RemovableMedia   = FALSE;
  mMedia.MediaPresent     = TRUE;
  mMedia.LogicalPartition = FALSE;
  mMedia.ReadOnly         = TRUE;
  mMedia.WriteCaching     = FALSE;
  mMedia.BlockSize        = RAM_BLOCK_SIZE;
  mMedia.IoAlign          = 1;
  mMedia.LastBlock        = (End - Start) / RAM_BLOCK_SIZE - 1;

  Handle = NULL;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiBlockIoProtocolGuid,
                  &mBlockIo,
                  NULL
                  );
  DEBUG ((DEBUG_ERROR, DEBUG_TAG "initrd block device published: %r\n", Status));
}

EFI_STATUS
EFIAPI
E5PayloadInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *Hob;
  UINT64             Fdt;

  Hob = GetFirstGuidHob (&gSiliciumEntryArgHobGuid);
  if (Hob == NULL) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "SEC left no entry-argument HOB\n"));
    return EFI_SUCCESS;
  }

  Fdt = *(UINT64 *)GET_GUID_HOB_DATA (Hob);
  if ((Fdt == 0) || ((Fdt & 3) != 0) || (FdtCheckHeader ((VOID *)(UINTN)Fdt) != 0)) {
    DEBUG ((DEBUG_ERROR, DEBUG_TAG "x0 0x%lx is not a device tree\n", Fdt));
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, DEBUG_TAG "device tree at 0x%lx, %u bytes\n", Fdt, (UINT32)FdtTotalSize ((VOID *)(UINTN)Fdt)));
  PublishInitrd ((VOID *)(UINTN)Fdt);
  return EFI_SUCCESS;
}
