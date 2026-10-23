/*
 *  BootShim exception reporter for the Rongyue E5 (Unisoc UMS9621).
 *
 *  Silicium's SEC never installs its own vector table (SecVectorTable exists in
 *  SiliciumPkg/Sec/AArch64/Exception.S but nothing in the tree ever writes
 *  VBAR), so any fault inside SEC lands on the vectors LK left behind and LK's
 *  panic path resets the SoC without a trace.  BootShim.S installs the vectors
 *  below at 0x80080000 before it branches into SEC, so they stay live for the
 *  whole window in which SEC runs without its own.
 *
 *  Everything here runs after the machine has already failed.  The three jobs,
 *  in this order: paint the panel red, print the fault on the panel, publish
 *  the same record to both ramoops zones.  Then spin on wfe and let the AP
 *  watchdog reset the board, so Android can read the record back.
 *
 *  Built with -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only: no libc,
 *  no FP/SIMD (the fault may be taken before SEC has enabled them), and no
 *  runtime relocation in a fixed-address image.  Also -mstrict-align: the MMU
 *  is off here, so every access is Device memory where an unaligned store is an
 *  alignment fault.  The compiler otherwise emits "stur x8, [sp, #4]" for the
 *  record buffer and the handler faults on its own stack -- measured in qemu.
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "E5Font8x16.h"

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long      UINTN;

/*  Panel: the DPU keeps scanning 0xbe000000 at 320x480 RGB565 with 640 byte
 *  rows after LK hands off, so a bare metal write is visible.  */
#define FB_BASE    0xbe000000UL
#define FB_WIDTH   320UL
#define FB_HEIGHT  480UL
#define FB_STRIDE  640UL
#define FB_PIXELS  (FB_WIDTH * FB_HEIGHT)

#define FB_RED     0xf800U    /* exception colour */
#define FB_TEXT    0xffffU    /* white on red */

#define FB_COLS    40
#define FB_ROWS    30

/*  ramoops: a 12 byte header (magic, start, length) followed by the payload. */
#define CONSOLE_ZONE  0xfffb0000UL
#define DMESG_ZONE    0xfff80000UL
#define PRZ_SIG       0x43474244UL     /* "DBGC" little endian */
#define PRZ_HDR       12UL
#define DMESG_PREFIX  "====0.000000-0\n"

/*  Vector index -> name.  index = group * 4 + kind.  */
static const char *KindName (UINTN Index)
{
  switch (Index & 3) {
  case 0: return "sync";
  case 1: return "irq";
  case 2: return "fiq";
  default: return "serr";
  }
}

static const char *GroupName (UINTN Index)
{
  switch (Index >> 2) {
  case 0: return "cur-sp0";
  case 1: return "cur-spx";
  case 2: return "lo-a64";
  default: return "lo-a32";
  }
}

static char *PutStr (char *Out, const char *Str)
{
  while (*Str != '\0') {
    *Out++ = *Str++;
  }
  return Out;
}

static char *PutDec (char *Out, UINT32 Value)
{
  char  Digits[10];
  int   Count;

  if (Value == 0) {
    *Out++ = '0';
    return Out;
  }
  Count = 0;
  while (Value != 0) {
    Digits[Count++] = (char)('0' + (Value % 10));
    Value /= 10;
  }
  while (Count != 0) {
    *Out++ = Digits[--Count];
  }
  return Out;
}

static char *PutHex (char *Out, UINTN Value)
{
  static const char Hex[] = "0123456789abcdef";
  int               Shift;

  *Out++ = '0';
  *Out++ = 'x';
  for (Shift = 60; Shift >= 0; Shift -= 4) {
    *Out++ = Hex[(Value >> Shift) & 0xf];
  }
  return Out;
}

/*  Push a written range out to the point of coherency.  Harmless with caches
 *  off, and the difference between "the panel shows the fault" and "the panel
 *  shows nothing" if SEC died after enabling caches.  */
static void CleanRange (UINTN Base, UINTN Length)
{
  UINTN Ctr;
  UINTN Line;
  UINTN Addr;

  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (Ctr));
  Line = 4UL << ((Ctr >> 16) & 0xf);      /* DminLine, log2 of words */
  for (Addr = Base & ~(Line - 1); Addr < Base + Length; Addr += Line) {
    __asm__ volatile ("dc civac, %0" : : "r" (Addr) : "memory");
  }
  __asm__ volatile ("dsb sy" : : : "memory");
}

static void FbFill (UINT16 Colour)
{
  volatile UINT16  *Pixel;
  UINTN            Index;

  Pixel = (volatile UINT16 *)FB_BASE;
  for (Index = 0; Index < FB_PIXELS; Index++) {
    Pixel[Index] = Colour;
  }
}

static void FbGlyph (int Col, int Row, char Ch)
{
  const UINT8     *Glyph;
  volatile UINT16 *Pixel;
  int             X;
  int             Y;

  if ((Ch < FONT_FIRST) || (Ch > FONT_LAST)) {
    Ch = '?';
  }
  Glyph = &font8x16[(Ch - FONT_FIRST) * 16];
  for (Y = 0; Y < 16; Y++) {
    Pixel = (volatile UINT16 *)(FB_BASE + ((UINTN)Row * 16 + Y) * FB_STRIDE
                                + (UINTN)Col * 16);
    for (X = 0; X < 8; X++) {
      Pixel[X] = (Glyph[Y] & (0x80U >> X)) ? FB_TEXT : FB_RED;
    }
  }
}

static void FbPrint (const char *Text)
{
  int Col;
  int Row;

  Col = 0;
  Row = 0;
  while (*Text != '\0') {
    if (*Text == '\n') {
      Col = 0;
      Row++;
      Text++;
      continue;
    }
    if (Col >= FB_COLS) {
      Col = 0;
      Row++;
    }
    if (Row >= FB_ROWS) {
      return;
    }
    FbGlyph (Col, Row, *Text);
    Col++;
    Text++;
  }
}

/*  hdr[0] = magic, hdr[1] = start, hdr[2] = length, payload at zone + 12.  */
static void Publish (UINTN Zone, const char *Prefix, const char *Text, UINTN Length)
{
  volatile UINT32 *Hdr;
  volatile UINT8  *Dst;
  UINTN           Index;

  Hdr = (volatile UINT32 *)Zone;
  Dst = (volatile UINT8 *)(Zone + PRZ_HDR);
  Index = 0;
  while (*Prefix != '\0') {
    Dst[Index++] = (UINT8)*Prefix++;
  }
  while (Length-- != 0) {
    Dst[Index++] = (UINT8)*Text++;
  }
  Hdr[0] = PRZ_SIG;
  Hdr[1] = 0;
  Hdr[2] = (UINT32)Index;
  CleanRange (Zone, PRZ_HDR + Index);
}

/*
 *  Called from the vector stub in BootShim.S with
 *
 *    x0 = vector index   x1 = ELR   x2 = ESR   x3 = FAR
 *    x4 = SPSR           x5 = SP    x6 = CurrentEL (1 or 2)
 *    x7 = number of times the handler has been entered, this one included
 *
 *  Stage 1 is the normal case.  A higher stage means a previous attempt
 *  faulted -- typically the panel is no longer mapped after SEC enabled its
 *  MMU -- so the panel is skipped and only the ramoops channels are tried.
 *  Beyond that nothing is left worth trying.
 *
 *  Never returns.
 */
void ShimExceptionReport (
  UINTN Type,
  UINTN Elr,
  UINTN Esr,
  UINTN Far,
  UINTN Spsr,
  UINTN Sp,
  UINTN El,
  UINTN Stage
  )
{
  char  Text[320];
  char  *Out;

  Out  = PutStr (Text, "\n==== SHIM EXC ====\ntype=");
  Out  = PutDec (Out, (UINT32)Type);
  *Out++ = ' ';
  Out  = PutStr (Out, GroupName (Type));
  *Out++ = '/';
  Out  = PutStr (Out, KindName (Type));
  Out  = PutStr (Out, " el=");
  Out  = PutDec (Out, (UINT32)El);
  Out  = PutStr (Out, "\nelr=");
  Out  = PutHex (Out, Elr);
  Out  = PutStr (Out, "\nesr=");
  Out  = PutHex (Out, Esr);
  Out  = PutStr (Out, "\nfar=");
  Out  = PutHex (Out, Far);
  Out  = PutStr (Out, "\nspsr=");
  Out  = PutHex (Out, Spsr);
  Out  = PutStr (Out, "\nsp=");
  Out  = PutHex (Out, Sp);
  *Out++ = '\n';
  *Out   = '\0';

  if (Stage > 1) {
    for ( ; ; ) {
      __asm__ volatile ("wfe");
    }
  }

  /*  Panel first: it needs nothing but the DPU.  */
  if (Stage == 0) {
    FbFill (FB_RED);
    FbPrint (Text);
    CleanRange (FB_BASE, FB_PIXELS * 2);
  }

  /*  Then both ramoops channels.  The console zone is shared with SEC's
   *  SerialPortLib output and this record overwrites it, which is intended:
   *  SEC's five lines are already known.  The dmesg zone needs the "===="
   *  prefix or the Android driver drops the record.  */
  Publish (CONSOLE_ZONE, "", Text, (UINTN)(Out - Text));
  Publish (DMESG_ZONE, DMESG_PREFIX, Text, (UINTN)(Out - Text));

  for ( ; ; ) {
    __asm__ volatile ("wfe");
  }
}
