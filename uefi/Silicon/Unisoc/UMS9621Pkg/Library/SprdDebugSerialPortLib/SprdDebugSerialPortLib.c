/*
#
  Debug serial port for the E5 (Unisoc UMS9621).

  Two sinks, both measured working from bare metal (see the M0 report):

   1. UART1 at 0x20210000 -- the SoC console UART.  Stock firmware runs it at
      921600 8n1, mainline Linux at 115200.  The physical test points are not
      identified yet, so this sink is currently write-only and invisible.

   2. The ramoops console zone at 0xFFFB0000 -- a persistent-RAM buffer that
      survives a warm reset.  Android mounts it as
      /sys/fs/pstore/console-ramoops-0, so after the LK watchdog resets the
      board we can read the whole UEFI boot log with
        adb root; adb exec-out cat /sys/fs/pstore/console-ramoops-0
      This is the primary bring-up channel.

  The persistent_ram header is three little-endian u32: signature, start
  offset of the oldest byte, number of valid bytes.
#

*/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/SerialPortLib.h>

#include "E5Font8x16.h"

#define UART1_BASE           0x20210000ULL
#define UART_TXD             0x00
#define UART_LSR             0x08
#define UART_LSR_TX_OVER     (1U << 15)
#define UART_TX_TIMEOUT      256000U

#define PRZ_SIG              0x43474244U
#define PRZ_HDR              12U
#define CONSOLE_ZONE         0xFFFB0000ULL
#define CONSOLE_CAP          (0x8000U - PRZ_HDR)

STATIC UINT32  mConsolePos;
STATIC BOOLEAN mConsoleWrapped;

//
// Framebuffer console.
//
// The panel is the only output channel that needs no reboot to read.  LK
// leaves the DPU scanning out 0xbe000000 at 320x480 RGB565 with a 640 byte row
// pitch, and the backlight is already on when it jumps to us, so text written
// there is visible while the firmware is still running.  A hang then shows up
// as the last line on the screen instead of costing a power cycle.
//
// Geometry and pixel format measured from bare metal; see the M0 report.
//

#define FB_BASE     0xbe000000ULL
#define FB_WIDTH    320U
#define FB_HEIGHT   480U
#define FB_STRIDE   (FB_WIDTH * 2U)
#define FB_COLS     (FB_WIDTH / 8U)
#define FB_ROWS     (FB_HEIGHT / 16U)
#define FB_CELL_W   8U
#define FB_CELL_H   16U
#define FB_BG       0x0000U
#define FB_FG       0xffffU

STATIC UINT32   mFbRow;
STATIC UINT32   mFbCol;
STATIC BOOLEAN  mFbReady;

STATIC
VOID
FbFillSpan (
  IN UINT32  X,
  IN UINT32  Y,
  IN UINT32  Width,
  IN UINT16  Color
  )
{
  volatile UINT16  *Pixel;
  UINT32           Index;

  Pixel = (volatile UINT16 *)(UINTN)(FB_BASE + (UINT64)Y * FB_STRIDE + (UINT64)X * 2U);
  for (Index = 0; Index < Width; Index++) {
    Pixel[Index] = Color;
  }
}

STATIC
VOID
FbClear (
  VOID
  )
{
  UINT32  Y;

  for (Y = 0; Y < FB_HEIGHT; Y++) {
    FbFillSpan (0, Y, FB_WIDTH, FB_BG);
  }
}

STATIC
VOID
FbScroll (
  VOID
  )
{
  volatile UINT16  *Dst;
  volatile UINT16  *Src;
  UINT32           Count;
  UINT32           Y;

  for (Y = 0; Y < (FB_HEIGHT - FB_CELL_H); Y++) {
    Dst   = (volatile UINT16 *)(UINTN)(FB_BASE + (UINT64)Y * FB_STRIDE);
    Src   = (volatile UINT16 *)(UINTN)(FB_BASE + (UINT64)(Y + FB_CELL_H) * FB_STRIDE);
    Count = FB_WIDTH;
    while (Count != 0) {
      *Dst = *Src;
      Dst++;
      Src++;
      Count--;
    }
  }

  for (Y = FB_HEIGHT - FB_CELL_H; Y < FB_HEIGHT; Y++) {
    FbFillSpan (0, Y, FB_WIDTH, FB_BG);
  }
}

STATIC
VOID
FbGlyph (
  IN UINT32  Col,
  IN UINT32  Row,
  IN UINT8   Ch,
  IN UINT16  Fg
  )
{
  CONST UINT8  *Bits;
  UINT32       X;
  UINT32       Y;

  if ((Ch < FONT_FIRST) || (Ch > FONT_LAST)) {
    Ch = (UINT8)0x3f;
  }

  Bits = &font8x16[(Ch - FONT_FIRST) * 16];

  for (Y = 0; Y < FB_CELL_H; Y++) {
    for (X = 0; X < FB_CELL_W; X++) {
      volatile UINT16  *Pixel;

      Pixel = (volatile UINT16 *)(UINTN)(FB_BASE + (UINT64)(Row * FB_CELL_H + Y) * FB_STRIDE + (UINT64)(Col * FB_CELL_W + X) * 2U);
      if ((Bits[Y] & (UINT8)(0x80 >> X)) != 0) {
        *Pixel = Fg;
      } else {
        *Pixel = FB_BG;
      }
    }
  }
}

STATIC
VOID
FbPutc (
  IN CHAR8  C
  )
{
  UINT8  Ch;

  Ch = (UINT8)C;

  if ((Ch == 0x0a) || (Ch == 0x0d)) {
    mFbCol = 0;
    if (Ch == 0x0a) {
      mFbRow++;
    }
  } else if (Ch == 0x09) {
    mFbCol = (mFbCol + 4U) & ~3U;
  } else if ((Ch >= 0x20) && (Ch < 0x7f)) {
    FbGlyph (mFbCol, mFbRow, Ch, FB_FG);
    mFbCol++;
  } else {
    return;
  }

  if (mFbCol >= FB_COLS) {
    mFbCol = 0;
    mFbRow++;
  }

  while (mFbRow >= FB_ROWS) {
    FbScroll ();
    mFbRow--;
  }
}

STATIC
VOID
FbConWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (mFbReady == 0) {
    FbClear ();
    mFbRow   = 0;
    mFbCol   = 0;
    mFbReady = TRUE;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    FbPutc ((CHAR8)Buffer[Index]);
  }
}

STATIC
VOID
UartPutc (
  IN CHAR8  C
  )
{
  UINT32  Timeout;

  for (Timeout = 0; Timeout < UART_TX_TIMEOUT; Timeout++) {
    if ((*(volatile UINT32 *)(UINTN)(UART1_BASE + UART_LSR) & UART_LSR_TX_OVER) != 0) {
      break;
    }
  }

  *(volatile UINT32 *)(UINTN)(UART1_BASE + UART_TXD) = (UINT32)(UINT8)C;
}

STATIC
VOID
ConsolePublish (
  VOID
  )
{
  volatile UINT32  *Header;

  Header    = (volatile UINT32 *)(UINTN)CONSOLE_ZONE;
  Header[0] = PRZ_SIG;
  Header[1] = mConsoleWrapped ? mConsolePos : 0;
  Header[2] = mConsoleWrapped ? CONSOLE_CAP : mConsolePos;
}

STATIC
VOID
ConsolePutc (
  IN CHAR8  C
  )
{
  volatile UINT8  *Data;

  Data = (volatile UINT8 *)(UINTN)(CONSOLE_ZONE + PRZ_HDR);

  if (mConsolePos >= CONSOLE_CAP) {
    mConsolePos      = 0;
    mConsoleWrapped  = TRUE;
  }

  Data[mConsolePos] = (UINT8)C;
  mConsolePos++;
}

/**
  Reset the persistent RAM header.  Called before the first byte is written so
  that a previous boot log can never be mistaken for the current one.
**/
RETURN_STATUS
EFIAPI
SprdDebugSerialPortLibConstructor (
  VOID
  )
{
  mConsolePos     = 0;
  mConsoleWrapped = FALSE;
  ConsolePublish ();
  return RETURN_SUCCESS;
}

/**
  Initialize the serial device hardware.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
  mConsolePos     = 0;
  mConsoleWrapped = FALSE;
  ConsolePublish ();
  return RETURN_SUCCESS;
}

/**
  Write data to the serial device.
**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8  *Buffer,
  IN UINTN  NumberOfBytes
  )
{
  UINTN  Index;

  if (Buffer == NULL) {
    return 0;
  }

  for (Index = 0; Index < NumberOfBytes; Index++) {
    UartPutc ((CHAR8)Buffer[Index]);
    ConsolePutc ((CHAR8)Buffer[Index]);
  }

  FbConWrite (Buffer, NumberOfBytes);
  ConsolePublish ();
  return NumberOfBytes;
}

/**
  Read data from the serial device.  Not supported: this port is write-only.
**/
UINTN
EFIAPI
SerialPortRead (
  OUT UINT8  *Buffer,
  IN  UINTN  NumberOfBytes
  )
{
  return 0;
}

/**
  Poll the serial device for input.  Always FALSE: this port is write-only.
**/
BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
  return FALSE;
}

RETURN_STATUS
EFIAPI
SerialPortSetControl (
  IN UINT32  Control
  )
{
  return RETURN_UNSUPPORTED;
}

RETURN_STATUS
EFIAPI
SerialPortGetControl (
  OUT UINT32  *Control
  )
{
  if (Control == NULL) {
    return RETURN_INVALID_PARAMETER;
  }

  *Control = 0;
  return RETURN_SUCCESS;
}

RETURN_STATUS
EFIAPI
SerialPortSetAttributes (
  IN OUT UINT64              *BaudRate,
  IN OUT UINT32              *ReceiveFifoDepth,
  IN OUT UINT32              *Timeout,
  IN OUT EFI_PARITY_TYPE     *Parity,
  IN OUT UINT8               *DataBits,
  IN OUT EFI_STOP_BITS_TYPE  *StopBits
  )
{
  return RETURN_UNSUPPORTED;
}
