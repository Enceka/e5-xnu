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
