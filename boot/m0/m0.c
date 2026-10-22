/*
 * M0 bring-up stub for the E5 (Unisoc UMS9621 / qogirn6l).
 *
 * LK copies boot_b's kernel to 0x80080000 and jumps there with the vendor FDT in
 * x0. This stub reports what the later stages need to know: exception level,
 * MPIDR/MIDR/CNTFRQ, GIC topology, the FDT LK passed, the eMMC controller's
 * SDHCI capabilities, and whether PSCI will start the secondary cores.
 *
 * There is no serial console wired up on this board, so the report goes to the
 * ramoops console zone at 0xfffb0000 and the dmesg zone at 0xfff80000, both of
 * which survive the reset, so Android can read them back from /sys/fs/pstore/.
 * UART1 at 0x20210000 is tried as well but is not trusted.
 *
 * Two rules for this file: the MMU is off, so every access is Device memory and
 * must be naturally aligned; and clang will happily merge byte stores into
 * 8-byte stores, hence -mstrict-align and the aligned buffers.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;

#define GICD_BASE 0x12000000UL
#define GICR_BASE 0x12040000UL
#define GICD_TYPER 0x0004

#define UART1_BASE 0x20210000UL
#define UART_TXD 0x00
#define UART_STS1 0x0c
#define UART_LSR 0x08
#define UART_LSR_TX_OVER (1u << 15)

#define PRZ_SIG 0x43474244UL
#define DMESG_ZONE 0xfff80000UL
#define CONSOLE_ZONE 0xfffb0000UL
#define PRZ_HDR 12

static inline u16 rd16(u64 a) { return *(volatile u16 *)a; }
static inline u32 rd32(u64 a) { return *(volatile u32 *)a; }
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)a = v; }

static u64 rd_currentel(void) { u64 v; __asm__ volatile("mrs %0, CurrentEL" : "=r"(v)); return v; }
static u64 rd_mpidr(void) { u64 v; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(v)); return v; }
static u64 rd_midr(void) { u64 v; __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(v)); return v; }
static u64 rd_cntfrq(void) { u64 v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static u64 rd_cntvct(void) { u64 v; __asm__ volatile("isb; mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static u64 rd_sctlr(void) { u64 v; __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(v)); return v; }
static u64 rd_daif(void) { u64 v; __asm__ volatile("mrs %0, DAIF" : "=r"(v)); return v; }
static u64 rd_pfr0(void) { u64 v; __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(v)); return v; }
static u64 rd_mmfr0(void) { u64 v; __asm__ volatile("mrs %0, ID_AA64MMFR0_EL1" : "=r"(v)); return v; }
static u64 rd_vbar(void) { u64 v; __asm__ volatile("mrs %0, VBAR_EL1" : "=r"(v)); return v; }
static u64 rd_sctlr_el2(void) { u64 v; __asm__ volatile("mrs %0, SCTLR_EL2" : "=r"(v)); return v; }
static u64 rd_vbar_el2(void) { u64 v; __asm__ volatile("mrs %0, VBAR_EL2" : "=r"(v)); return v; }
static u64 rd_hcr_el2(void) { u64 v; __asm__ volatile("mrs %0, HCR_EL2" : "=r"(v)); return v; }
static u64 rd_cptr_el2(void) { u64 v; __asm__ volatile("mrs %0, CPTR_EL2" : "=r"(v)); return v; }

/* ---------- report buffer ---------- */

static char out[12288] __attribute__((aligned(16)));
static u32 outlen;

/* Secondaries format into their own slot and must not publish. */
static char *otgt = out;
static u32 ocap = sizeof(out);
static u32 olen;
static int publishing = 1;

static void sync_report(void);

static void o_putc(char c)
{
if (olen < ocap)
otgt[olen++] = c;
}

static void o_puts(const char *s)
{
while (*s)
o_putc(*s++);
}

static void o_hex(u64 v)
{
static const char d[] = "0123456789abcdef";
int shift = 60;

o_puts("0x");
while (shift > 0 && ((v >> shift) & 0xf) == 0)
shift -= 4;
for (; shift >= 0; shift -= 4)
o_putc(d[(v >> shift) & 0xf]);
}

static void o_dec(u64 v)
{
char buf[24];
int i = 0;

if (v == 0) {
o_putc('0');
return;
}
while (v) {
buf[i++] = (char)('0' + (v % 10));
v /= 10;
}
while (i--)
o_putc(buf[i]);
}

static void o_idec(long v)
{
if (v < 0) {
o_putc('-');
o_dec((u64)(-v));
} else {
o_dec((u64)v);
}
}

static void field(const char *name, u64 v)
{
o_puts("  ");
o_puts(name);
o_puts(" = ");
o_hex(v);
o_putc('\n');
sync_report();
}

static void dec_field(const char *name, u64 v)
{
o_puts("  ");
o_puts(name);
o_puts(" = ");
o_dec(v);
o_putc('\n');
sync_report();
}

static void line(const char *s)
{
o_puts("  ");
o_puts(s);
o_putc('\n');
sync_report();
}

/* ---------- sinks ---------- */

static u32 uart_ok = 1;
static u32 uart_sent;

static void uart_putc(char c)
{
u32 spin = 200000;

if (uart_ok == 0)
return;
while ((rd32(UART1_BASE + UART_LSR) & UART_LSR_TX_OVER) == 0) {
if (spin == 0) {
uart_ok = 0;
return;
}
spin--;
}
wr32(UART1_BASE + UART_TXD, (u32)(u8)c);
uart_sent++;
}

static void uart_flush(void)
{
while (uart_sent < olen) {
char c = out[uart_sent++];

if (c == '\n')
uart_putc('\r');
uart_putc(c);
}
}

static void prz_publish(u64 zone, const char *data, u32 len)
{
volatile u32 *hdr = (volatile u32 *)zone;
volatile u8 *dst = (volatile u8 *)(zone + PRZ_HDR);
u32 i;

hdr[0] = (u32)PRZ_SIG;
hdr[1] = 0;
hdr[2] = len;
for (i = 0; i < len; i++)
dst[i] = (u8)data[i];
__asm__ volatile("dsb sy" ::: "memory");
}

/* dmesg records need the "====" prefix or the driver zaps them. */
static void dmesg_publish(void)
{
static const char hdr[] = "====0.000000-0\n";
volatile u32 *h = (volatile u32 *)DMESG_ZONE;
volatile u8 *dst = (volatile u8 *)(DMESG_ZONE + PRZ_HDR);
u32 i, n = 0;

h[0] = (u32)PRZ_SIG;
h[1] = 0;
h[2] = (u32)(sizeof(hdr) - 1 + olen);
for (i = 0; hdr[i]; i++)
dst[n++] = (u8)hdr[i];
for (i = 0; i < olen; i++)
dst[n++] = (u8)out[i];
__asm__ volatile("dsb sy" ::: "memory");
}

static void sync_report(void)
{
if (publishing == 0)
return;
outlen = olen;
prz_publish(CONSOLE_ZONE, out, outlen);
dmesg_publish();
uart_flush();
}

/* ---------- PSCI ---------- */

static u64 psci_call(u64 fn, u64 a1, u64 a2)
{
register u64 r0 __asm__("x0") = fn;
register u64 r1 __asm__("x1") = a1;
register u64 r2 __asm__("x2") = a2;
register u64 r3 __asm__("x3") = 0;

__asm__ volatile("smc #0"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                 : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                     "x12", "x13", "x14", "x15", "x16", "x17", "memory");
return r0;
}

static u64 psci_call3(u64 fn, u64 a1, u64 a2, u64 a3)
{
register u64 r0 __asm__("x0") = fn;
register u64 r1 __asm__("x1") = a1;
register u64 r2 __asm__("x2") = a2;
register u64 r3 __asm__("x3") = a3;

__asm__ volatile("smc #0"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                 : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                     "x12", "x13", "x14", "x15", "x16", "x17", "memory");
return r0;
}

static void psci_reset(void)
{
(void)psci_call(0x84000009UL, 0, 0);
}

/* LK's EL2 vectors are still live, so a deliberate fault makes LK print our
 * registers and reboot the board for us. */
static void die(void)
{
(void)*(volatile u32 *)0x222000feUL;
for (;;)
__asm__ volatile("wfe");
}

static u64 psci_call3(u64 fn, u64 a1, u64 a2, u64 a3);

extern char sec_entry[];

/* ---------- secondary core bring-up ---------- */

#define SEC_SLOTS 16
u8 sec_stacks[SEC_SLOTS][0x1000] __attribute__((aligned(16)));
static char sec_out[SEC_SLOTS][192] __attribute__((aligned(16)));
static volatile u32 sec_done[SEC_SLOTS];

/* Local formatters: the secondary must not touch the primary's globals. */
static u32 s_put(char *b, u32 cap, u32 n, const char *s)
{
while (*s && n < cap)
b[n++] = *s++;
return n;
}

static u32 s_hex(char *b, u32 cap, u32 n, u64 v)
{
static const char d[] = "0123456789abcdef";
int shift = 60;

n = s_put(b, cap, n, "0x");
while (shift > 0 && ((v >> shift) & 0xf) == 0)
shift -= 4;
for (; shift >= 0; shift -= 4) {
if (n < cap)
b[n++] = d[(v >> shift) & 0xf];
}
return n;
}

static u32 s_dec(char *b, u32 cap, u32 n, u64 v)
{
char t[24];
int i = 0;

if (v == 0) {
if (n < cap)
b[n++] = '0';
return n;
}
while (v) {
t[i++] = (char)('0' + (v % 10));
v /= 10;
}
while (i--) {
if (n < cap)
b[n++] = t[i];
}
return n;
}

void sec_main(u64 slot, u64 mpidr)
{
char *b;
u32 n = 0;

if (slot >= SEC_SLOTS) {
for (;;)
__asm__ volatile("wfe");
}
b = sec_out[slot];
n = s_put(b, sizeof(sec_out[slot]), n, "core mp=");
n = s_hex(b, sizeof(sec_out[slot]), n, mpidr);
n = s_put(b, sizeof(sec_out[slot]), n, " el=");
n = s_dec(b, sizeof(sec_out[slot]), n, (rd_currentel() >> 2) & 3);
n = s_put(b, sizeof(sec_out[slot]), n, " midr=");
n = s_hex(b, sizeof(sec_out[slot]), n, rd_midr());
n = s_put(b, sizeof(sec_out[slot]), n, " frq=");
n = s_dec(b, sizeof(sec_out[slot]), n, rd_cntfrq());
n = s_put(b, sizeof(sec_out[slot]), n, " gicd=");
n = s_hex(b, sizeof(sec_out[slot]), n, rd32(GICD_BASE + GICD_TYPER));
if (n < sizeof(sec_out[slot]))
b[n++] = '\n';
if (n < sizeof(sec_out[slot]))
b[n] = 0;
__asm__ volatile("dmb sy" ::: "memory");
sec_done[slot] = 1;
__asm__ volatile("dmb sy" ::: "memory");
/* Park with PSCI CPU_OFF so the core does not wander. */
(void)psci_call(0x84000002UL, 0, 0);
for (;;)
__asm__ volatile("wfe");
}

/* ---------- probes ---------- */

#define GICD_CTLR 0x0000
#define GICD_TYPER 0x0004
#define GICD_IIDR 0x0008
#define GICD_PIDR0 0x0fe0
#define GICD_PIDR2 0x0fe8
#define GICR_TYPER 0x0008
#define GICR_WAKER 0x0014
#define GICR_STRIDE 0x20000UL

static void probe_gic(void)
{
u32 ctlr = rd32(GICD_BASE + GICD_CTLR);
u32 typer = rd32(GICD_BASE + GICD_TYPER);
u64 base = GICR_BASE;
int i;

o_puts("GIC:\n");
field("GICD_CTLR", ctlr);
field("GICD_TYPER", typer);
dec_field("  ITLines/SPI lines", (((typer & 0x1f) + 1) * 32));
field("  ESPI (bit10)", (typer >> 10) & 1);
field("  LPI support (bit16)", (typer >> 16) & 1);
field("  SecurityExtn (bit20)", (typer >> 20) & 1);
field("GICD_IIDR", rd32(GICD_BASE + GICD_IIDR));
field("GICD_PIDR0", rd32(GICD_BASE + GICD_PIDR0));
field("GICD_PIDR2 arch rev", (rd32(GICD_BASE + GICD_PIDR2) >> 4) & 0xf);

for (i = 0; i < 12; i++) {
u32 lo = rd32(base + GICR_TYPER);
u32 hi = rd32(base + GICR_TYPER + 4);

o_puts("  GICR");
o_dec((u64)i);
o_puts(" typer=");
o_hex(((u64)hi << 32) | lo);
o_puts(" aff=");
o_hex(lo & 0xffffff);
o_puts(" pn=");
o_dec((lo >> 24) & 0x1f);
o_puts(" last=");
o_dec(hi & 1);
o_puts(" ctlr=");
o_hex(rd32(base + 0x0000));
o_puts(" iidr=");
o_hex(rd32(base + 0x0004));
o_puts(" waker=");
o_hex(rd32(base + GICR_WAKER));
o_puts(" prop=");
o_hex(rd32(base + 0x0070));
o_puts(" pend=");
o_hex(rd32(base + 0x0078));
o_putc('\n');
sync_report();
base += GICR_STRIDE;
}
}

#define SDHCI_BASE 0x22200000UL
#define SDHCI_HOST_VERSION 0xfe
#define SDHCI_CAPABILITIES 0x40
#define SDHCI_CAPABILITIES_1 0x44

static void probe_emmc(void)
{
u16 version = rd16(SDHCI_BASE + SDHCI_HOST_VERSION);
u32 caps = rd32(SDHCI_BASE + SDHCI_CAPABILITIES);
u32 caps1 = rd32(SDHCI_BASE + SDHCI_CAPABILITIES_1);

o_puts("eMMC sdhci@22200000:\n");
field("HOST_VERSION", version);
dec_field("  spec version", version & 0xff);
field("CAPABILITIES", caps);
field("CAPABILITIES_1", caps1);
dec_field("  base clock MHz [15:8]", (caps >> 8) & 0xff);
field("  VDD 3.3V", (caps & 0x01000000UL) ? 1 : 0);
field("  VDD 3.0V", (caps & 0x02000000UL) ? 1 : 0);
field("  VDD 1.8V", (caps & 0x04000000UL) ? 1 : 0);
field("  high speed", (caps & 0x00200000UL) ? 1 : 0);
field("  8-bit bus", (caps & 0x00040000UL) ? 1 : 0);
field("  ADMA2", (caps & 0x00080000UL) ? 1 : 0);
field("  ADMA3", (caps1 & 0x08000000UL) ? 1 : 0);
field("  64-bit addr", (caps & 0x10000000UL) ? 1 : 0);
field("  SDR50", (caps1 & 0x1UL) ? 1 : 0);
field("  SDR104", (caps1 & 0x2UL) ? 1 : 0);
field("  DDR50", (caps1 & 0x4UL) ? 1 : 0);
field("  HS400", (caps1 & 0x80000000UL) ? 1 : 0);
}

/* The FDT is big-endian; the CPU is little-endian. */
static u32 bswap32(u32 v)
{
return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | ((v >> 24) & 0xff);
}

static void probe_fdt(u64 fdt)
{
o_puts("FDT passed in x0:\n");
field("address", fdt);
field("magic (want 0xd00dfeed)", bswap32(rd32(fdt)));
field("totalsize", bswap32(rd32(fdt + 4)));
field("off_dt_struct", bswap32(rd32(fdt + 8)));
field("off_dt_strings", bswap32(rd32(fdt + 12)));
field("version", bswap32(rd32(fdt + 20)));
field("size_dt_strings", bswap32(rd32(fdt + 32)));
field("size_dt_struct", bswap32(rd32(fdt + 36)));
}

static void probe_psci(void)
{
static const u64 aff[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x100, 0x101};
u32 i;

o_puts("PSCI:\n");
field("PSCI_VERSION", psci_call(0x84000000UL, 0, 0));
o_puts("  AFFINITY_INFO (0=ON 1=OFF 2=PENDING -1=INVALID -2=NOT_SUP):\n");
sync_report();
for (i = 0; i < sizeof(aff) / sizeof(aff[0]); i++) {
long r = (long)(int)(u32)psci_call(0x84000004UL, aff[i], 0);

o_puts("    affinity ");
o_hex(aff[i]);
o_puts(" -> ");
o_idec(r);
o_putc('\n');
sync_report();
}
}

static void probe_smp(void)
{
static const u64 aff[] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x100, 0x101};
u32 i, s;

o_puts("SMP (PSCI CPU_ON -> sec_entry):\n");
sync_report();
for (i = 0; i < sizeof(aff) / sizeof(aff[0]); i++) {
long r = (long)(int)(u32)psci_call3(0x84000003UL, aff[i],
                                    (u64)(void *)sec_entry, 0);
u32 waited = 0;
u32 slot = (u32)(aff[i] & 0xff) | (((u32)(aff[i] >> 8) & 1) << 3);

while (sec_done[slot] == 0 && waited < 20000000) {
__asm__ volatile("yield");
waited++;
}
o_puts("  target ");
o_hex(aff[i]);
o_puts(" -> psci ");
o_idec(r);
o_puts(" slot ");
o_dec((u64)slot);
if (sec_done[slot]) {
o_puts(" reported\n");
sync_report();
line(sec_out[slot]);
} else {
o_puts(" silent\n");
sync_report();
}
}
for (s = 0; s < SEC_SLOTS; s++) {
if (sec_done[s])
continue;
}
}

static void probe_uart(void)
{
o_puts("UART1 0x20210000:\n");
field("LSR", rd32(UART1_BASE + UART_LSR));
field("STS1 (TX/RX fifo counts)", rd32(UART1_BASE + UART_STS1));
dec_field("bytes accepted", uart_sent);
dec_field("still draining", uart_ok);
}

/* ---------- main ---------- */

extern void drop_to_el1(u64 fn, u64 sp, u64 vbar);
extern char el1_vectors[];
extern char __stack_top[];
extern char sec_entry[];

void m0_el1_main(void)
{
o_puts("\n===== M0-E5-AT-EL1 =====\n");
field("CurrentEL (want 0x4)", rd_currentel());
field("SCTLR_EL1", rd_sctlr());
field("MPIDR_EL1", rd_mpidr());
field("MIDR_EL1", rd_midr());
field("VBAR_EL1", rd_vbar());
dec_field("CNTFRQ_EL0 Hz", rd_cntfrq());
field("CNTVCT_EL0", rd_cntvct());
field("GICD_TYPER read at EL1", rd32(GICD_BASE + GICD_TYPER));
o_puts("===== M0-E5-EL1-DONE =====\n");
sync_report();

psci_reset();
die();
}

void m0_main(u64 x0, u64 x1, u64 x2, u64 x3)
{
u64 mpidr, midr, pfr0, mmfr0;

o_puts("\n\n===== M0-E5-BEGIN =====\n");
o_puts("E5 M0 stub, LK handoff report\n");

/* Insurance: an EL1 fault should land in our own pad, not at address 0. */
__asm__ volatile("msr vbar_el1, %0" :: "r"((u64)(void *)el1_vectors));
__asm__ volatile("isb");

field("CurrentEL", rd_currentel());

mpidr = rd_mpidr();
field("MPIDR_EL1", mpidr);
dec_field("  aff0", mpidr & 0xff);
dec_field("  aff1", (mpidr >> 8) & 0xff);
dec_field("  aff2", (mpidr >> 16) & 0xff);

midr = rd_midr();
field("MIDR_EL1", midr);
field("  implementer", (midr >> 24) & 0xff);
field("  variant", (midr >> 20) & 0xf);
field("  partnum", (midr >> 4) & 0xfff);
field("  revision", midr & 0xf);

field("SCTLR_EL1", rd_sctlr());
field("  MMU on (M)", rd_sctlr() & 1);
field("  D-cache on (C)", (rd_sctlr() >> 2) & 1);
field("  I-cache on (I)", (rd_sctlr() >> 12) & 1);
field("DAIF", rd_daif());
field("VBAR_EL1", rd_vbar());
field("SCTLR_EL2", rd_sctlr_el2());
field("  MMU on (M)", rd_sctlr_el2() & 1);
field("HCR_EL2", rd_hcr_el2());
field("  RW (1 = EL1 is AArch64)", (rd_hcr_el2() >> 31) & 1);
field("VBAR_EL2", rd_vbar_el2());
field("CPTR_EL2", rd_cptr_el2());
field("  TFP (traps FP/SIMD)", (rd_cptr_el2() >> 10) & 1);
pfr0 = rd_pfr0();
mmfr0 = rd_mmfr0();
field("ID_AA64PFR0_EL1", pfr0);
field("  EL0", (pfr0 >> 0) & 0xf);
field("  EL1", (pfr0 >> 4) & 0xf);
field("  EL2", (pfr0 >> 8) & 0xf);
field("  EL3", (pfr0 >> 12) & 0xf);
field("  FP", (pfr0 >> 16) & 0xf);
field("  AdvSIMD", (pfr0 >> 20) & 0xf);
field("  GIC sysregs", (pfr0 >> 24) & 0xf);
field("ID_AA64MMFR0_EL1", mmfr0);
field("  PARange", (mmfr0 >> 0) & 0xf);
field("  ASID bits", (mmfr0 >> 4) & 0xf);
field("  TGran4 (0 = 4K ok)", (mmfr0 >> 28) & 0xf);
field("  TGran16 (0 = 16K ok)", (mmfr0 >> 20) & 0xf);
field("  TGran64 (0 = 64K ok)", (mmfr0 >> 24) & 0xf);
dec_field("CNTFRQ_EL0 Hz", rd_cntfrq());
field("CNTVCT_EL0", rd_cntvct());

o_puts("handoff registers:\n");
field("x0", x0);
field("x1", x1);
field("x2", x2);
field("x3", x3);

if (x0 >= 0x80000000UL && x0 < 0xc0000000UL)
probe_fdt(x0);
else
line("x0 is not a RAM address, skipping FDT probe");

probe_gic();
probe_emmc();
probe_psci();
probe_smp();
probe_uart();

o_puts("===== M0-E5-END (still at EL2) =====\n");
sync_report();

/* Last thing: prove the EL2 -> EL1 drop works, since XNU assumes EL1. */
drop_to_el1((u64)(void *)m0_el1_main, (u64)(void *)__stack_top,
            (u64)(void *)el1_vectors);

/* eret is not expected to come back */
die();
}
