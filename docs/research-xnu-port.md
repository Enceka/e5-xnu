# 把 XNU 移植到荣悦 E5（UMS9621）——可行性研究

本文是 `docs/research-e5-hardware.md`（硬件事实）的续篇：那份文档回答「这块板子上有什么」，
这份回答「XNU 需要什么、差多少、先做哪一步」。标记沿用【C】已确认 /【I】推断 /【U】未验证。

研究对象：UniXNU（GitHub 组织 UniXNU，主仓 611 star，BSD-2-Clause-Patent）、
Mu-Silicium（Project-Silicium/Mu-Silicium，561 star）、XNU 上游 `xnu-10063.141.1`（UniXNU feeds.toml 锁定的版本）。

## 摘要

**结论：可行，但不能「刷一个现成固件」，必须自己补三层地基。** 三个硬门槛按难度排序：

1. **没有 UEFI，而且没有 Unisoc 的 UEFI 移植可抄。** E5 的引导链是 bootrom → FDL → LK（"uboot" 分区）
   → 直接跳内核，没有 EDK2 的位置。Mu-Silicium 有 MediaTek / Qualcomm / Samsung / Nvidia 的 SoC 包，
   **没有 Unisoc/展锐**，所以 `Silicon/Unisoc/UMS9621Pkg` + `Platforms/Unisoc/e5Pkg` 要从零写。
2. **XNU 侧没有任何 eMMC/存储驱动。** UniXNU 现有 `NativeUfs`（UFS）与 `NativeVirtioBlock`（virtio），
   而 E5 是 SDHCI 类 eMMC（`sprd,sdhci-r11`）且**无 UFS 节点**。root 卷要自己写块驱动。
3. **GPU 不可用。** E5 是 Mali-G57（"sprd,mali-natt"），UniXNU 的 `NativeAdreno` / `NativeMetal` /
   AdrenoPM4 / AirNir 全套是 Qualcomm Adreno 专用，**一行都复用不了**。第一阶段只能串口 + 软件帧缓冲。

**能原样复用的部分（这才是可行性所在）：** XNU 本体的 arm64 移植层（`osfmk/arm64/start.s` 起的整条
bootstrap）、`ArmCpu`（GICv3 + PSCI + 架构定时器，本机 PSCI 0.2 / GICv3 完全对得上）、
`ArmDTPlatform`（ADT → IORegistry）、`DarwinBoot`（Mach-O 载入 + 页表 + trust cache）、
UniXNU 的 53 个 XNU 补丁、以及 `NativeRamBlock` / `NativeKeyStoreEvents` / `NativeHidInput` 这类通用件。

**推荐路线：** Mu-Silicium 式 EDK2 移植（阶段 1 只做串口，不做显示）→ UniXNU → DarwinBoot → XNU。
先用一个 200 行的裸机 stub 验证 LK 的交接契约（M0），把「LK 到底给了什么」实测清楚，再动固件。

## 一、本机新增实测（在 hardware 文档之外）

`dumps/live-fdt.dtb`（Android 运行时 `/sys/firmware/fdt`，285820 字节）已解析成 `dumps/live-fdt.dts`（7478 行）。

- **GICv3**【C】：`/interrupt-controller@12000000`，`compatible = "arm,gic-v3"`，
  `reg = <0 0x12000000 0 0x20000  0 0x12040000 0 0x100000>`（GICD 128KiB + GICR 1MiB），
  `redistributor-stride = <0 0x20000>`、`#redistributor-regions = <1>`、`#interrupt-cells = <3>`、
  自身维护中断 `interrupts = <1 9 4>`（PPI 9）。1MiB GICR / 128KiB stride = 8 个 redistributor，与 8 核吻合。
- **架构定时器**【C】：`/timer`，`compatible = "arm,armv8-timer"`，
  `interrupts = <1 13 4  1 14 4  1 11 4  1 10 4>`（secure-phys 13 / non-secure-phys 14 / virt 11 / hyp 10），
  带 `arm,no-tick-in-suspend`。节点里**没有 `clock-frequency`**，频率由固件写进 `CNTFRQ_EL0`【I】。
- **chosen**【C】：`stdout-path = "serial1:115200n8"`（LK 把 stock 的 921600 改成了 115200），
  `linux,initrd-start = 0xc0000000`、`linux,initrd-end = 0xc3640644`（54.2MiB，即 vendor_boot 的
  vendor_ramdisk 56525040 字节 + bootconfig）-> **LK 把 ramdisk 固定装在 0xc0000000**。
- **PMU**【C】：`/pmu`，`compatible = "arm,armv8-pmuv3"`，SPI 177-184。
- **LK 日志证据**【C】（`dumps/uboot_log.bin`，16MiB 环形日志，多轮启动）：
  - `booting 2 cpus`、`platform: qogirn6l`、`target: ums9158_1h10`、`buildid: 2024-11-28-22:32:12_LOCAL`。
  - `ARG_START_BASE: 0x98100000, VBMETA_IMG_BASE: 0x99000000, VERIFY_BASE: 0x99800000`；
    `bootimage and recovery addr is 0x88000000, buf_size is 0x28000000`。
  - `generic kernel size is 46754304`、`kernel offset is 0x1000`、`generic ramdisk size is 360840`、
    `ramdisk offset is 0x2c98000`、`The kernel is in raw format`、
    `boot_a kernel read OK, page size = 0x2c97000, locate to 0x80080000`。
  - 历史记录里 `boot_b kernel read OK, page size = 0x11cb000 / 0x202c000 / 0x1dd1000`，全部 `locate to 0x80080000`
    —— 无论给多大的 kernel，LK 都装到 0x80080000，这正是 hardware 文档里 wrap-image stub 方案成立的原因。
  - `[start_linux] start linux at PC:0x80080000, dt_addr:0x86000000`；
    `/memory nodeoffset = 212004, ddr_base: 0x80000000, ddr_size: 0x7ffff000`。
  - DTB 选择：`Find kernel dtb prop name sprd,sc-id=ums9621 1000 1000` -> `Matches the corresponding dtb in multiple DTB`；
    dtbo `matches dtbo entry(id:0x1800)`、`merge_dtbo : entry id is 0x1800, offset = 0x60 size=0x1cf8b`。
  - AVB：vbmeta_a `Error verifying vbmeta image: HASH_MISMATCH`，但随后 `WARNNING: LOCK FLAG IS : UNLOCK, SKIP VERIFY`，
    `avb_slot_verify result is 3 / ERROR_VERIFICATION`、`veritymode=disabled`
    -> **改 boot_a/boot_b 不会被拦，LK 只打警告**【C】。
  - 耗时：`lk consume time: 8177ms`（含 `kernel_dtb_parse ~1273ms`、`secure_sign_verify 905ms`），LK 阶段约 8-9 秒【C】。
  - 分区：`boot_a`/`boot_b` 各 65536KiB（64MiB），`vendor_boot_*` 102400KiB，`super` 5734400KiB，`userdata` 到末尾【C】。

## 二、XNU 的入口契约（与 Linux 完全不同）

### 2.1 寄存器约定

Linux arm64 boot protocol：`x0` = DTB 物理地址，`x1..x3` = 0，EL1/EL2，MMU 关，镜像 4KiB 对齐。

XNU：`osfmk/arm64/start.s:223` 的 `_start` 只有一条 `b EXT(start_first_cpu)`；
`start_first_cpu`（同文件 :456）把 **`x0` 当成 `struct boot_args` 的物理地址**，x1 起全部忽略。
它做的头几件事说明运行前提：`msr OSLAR_EL1, xzr`、`msr DAIFSet, #(DAIFSC_ALL)`、
`MSR_VBAR_EL1_X0`、`msr SPSel, #1` —— **必须是 EL1、MMU 关、中断关**。
`LowExceptionVectorBase` 紧跟在 `_start` 之后（`.align 12`），所以 `reset vector = (_start & ~0xfff)`，
PSCI CPU_ON 的 entry 就是这一页，副核从 `start_cpu` 进。

推论【C】：**XNU 不需要 UEFI**。任何能在 EL1 填好一个 `boot_args` 并跳过去的代码都能启动它。
这既是「200 行 stub 可以当 M0」的依据，也是「UEFI 不是唯一路线」的依据。

**M0 实测修正（见 `docs/m0-bringup-report.md`）**：LK 实际把控制权交在 **EL2**，且 `HCR_EL2.RW = 0`
（此刻 EL1 是 AArch32）。所以启动 XNU 之前必须自己降到 EL1：

```asm
msr   sp_el1, x1          // EL1 栈
msr   vbar_el1, x2        // EL1 向量表（XNU 会自己重设，先给一张保命）
msr   elr_el2, x0         // 目标地址 = start_first_cpu
mov   x9, #0x3c5          // EL1h, DAIF 全屏蔽
msr   spsr_el2, x9
mov   x9, #1
lsl   x9, x9, #31         // HCR_EL2.RW = 1
msr   hcr_el2, x9
msr   cptr_el2, xzr
isb
eret
```

这条路径已在实机上跑通（`CurrentEL` 从 `0x8` 变 `0x4`）【C】。

`struct boot_args`（`pexpert/pexpert/arm64/boot.h:56-77`）要填的字段：

```c
uint16_t Revision, Version;          /* kBootArgsRevision=1, kBootArgsVersion2=2 */
uint64_t virtBase, physBase, memSize, topOfKernelData;
Boot_Video Video;                    /* 6 个 unsigned long：base/display/rowBytes/w/h/depth */
uint32_t machineType;
void    *deviceTreeP;                /* 物理地址，ADT 不是 FDT */
uint32_t deviceTreeLength;
char     CommandLine[1024];
uint64_t bootFlags, memSizeActual;
```

Apple 侧由 iBoot 填，非 Apple 侧由 UniXNU 的 `DarwinBoot` 填（它是 UEFI app，负责 Mach-O 载入、
页表、trust cache、`RAMDisk`/`PackedRAMDisk`/`TrustCache` 三块内存，最后 `start_el1`）。
**注意 `deviceTreeP` 指向 ADT**，把 vendor FDT 直接塞进去 XNU 会立刻崩在 platform expert 里。

### 2.2 两棵设备树：UEFI 用 FDT，XNU 用 ADT

UniXNU 文档 `docs/porting/device-tree.md` 讲得很清楚：

| 树 | 格式 | 消费者 |
| --- | --- | --- |
| UEFI device tree | 标准 FDT（dtc/libfdt） | UEFI 固件、EFI/Linux 条目 |
| Apple device tree (ADT) | Apple 私有紧凑编码 | XNU platform expert 与驱动 |

ADT 编码：`u32 property_count; u32 child_count;` 然后每个属性 `char name[32]; u32 length; u8 value[length]`
（4 字节对齐），再递归子节点；每个节点带一个含自身名字加 NUL 的 `name` 属性。

`ArmDTPlatform` 要求下列顶层节点，缺一个 platform expert 的 `configure()` 就失败：

| 节点 | 必需内容 |
| --- | --- |
| `/chosen` | `random-seed`（64 字节）、CHRP NVRAM proxy、DRAM 描述、`osenvironment` |
| `/cpus` | 每核一个 `cpuN` |
| `/arm-io` | 总线节点，带 `#address-cells`、`#size-cells`、`ranges` |
| `/defaults` | 平台默认值 |
| `/options` | 平台选项 |

对本机直接可用的映射【C】：

- `/arm-io/intc`：`compatible = "arm,gic-v3"`，`reg = <0 0x12000000 0 0x20000 0 0x12040000 0 0x100000>`，
  `redistributor-stride` 见 2.3；`/arm-io` 的 `ranges` 必须覆盖 0x12000000，否则子节点表达不出来。
- `/cpus/cpu0..cpu7`：`reg` 填 MPIDR 亲和值（A55 ×6 + A76 ×2 的 cluster/core 编码要在 M0 里实测 `MPIDR_EL1`），
  `compatible` 用 `arm,cortex-a55` / `arm,cortex-a76`，`timebase-frequency` 填 `CNTFRQ_EL0` 实测值。
- `/psci`：`method = "smc"`（vendor FDT 是 `arm,psci-0.2`）。ArmCpu 只读 `method` 属性，
  CPU_ON 用 `0xc4000003`，所以 0.2 与 1.0 的写法它都吃得下。
- `/chosen`：`dram-base = 0x80000000`、`dram-size` 要减掉 reserved-memory 里的 CP 436MiB 与 CMA 360MiB，
  `osenvironment = "main"`（先跑 ramdisk 阶段可用 `"ramdisk"`）、`unique-chip-id` / `chip-id` 用板子自己的值。
- `memory-map` 占位符：`RAMDisk`、`PackedRAMDisk`、`TrustCache` 各 16 字节，缺一个 DarwinBoot 返回
  `EFI_COMPROMISED_DATA`。
- NVRAM proxy 可以是空 bank（本机没有持久 NVRAM 控制器），但必须有，否则 configure 失败。
- `/defaults` 里先放 `sleep-disabled = <1>`（我们没有 suspend 实现）。

### 2.3 中断 / 定时器 / SMP：ArmCpu 与本机硬件对得上

UniXNU 的 `ArmCpu` 是「GICv3 + PSCI + 架构定时器」的现成驱动，读它的源码可以逐条核对：

- 控制器识别：`IORegistryEntry::fromPath("/arm-io/intc")`，认 `arm,gic-v3`，`redistributor-stride` 从 ADT 读
  （本机 0x20000）。SPI 数量从 GICD_TYPER 算：`unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;`（ArmCpu.cpp:269）。
- 定时器：`mrs %0, cntfrq_el0`（ArmCpu.cpp:135）取频率；PPI 27 作为定时器中断；不需要外部 timebase 表。
- IPI：SGI 0 作为核间中断；`kIpiSpiBase = 1008`（ArmCpu.cpp:92）把 SPI 1008-1015 映射成「发给逻辑核 0-7」。
- SMP：`psciConduit()`（ArmCpu.cpp:464-476）从 `/psci` 的 `method` 取 `smc`/`hvc`；
  `psciCall(hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0)`（ArmCpu.cpp:773）即 PSCI 0.2 CPU_ON。
  注释原文：「PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page」。
- 本机 vendor FDT 的 PSCI 是 0.2 / `method = "smc"`，GIC 是 GICv3 且 8 个 redistributor —— **完全匹配，ArmCpu 不用改**。

### 2.4 控制台：两处都要写驱动

- **XNU 早期控制台**：`pexpert` 的 `PE_putc` 是板级实现，BCM2837 走 `PI3_UART`（PL011 迷你驱动）。
  E5 要新增一个 sprd UART 后端，只写 `UART1@0x20210000`（115200n8）即可，几十行。
- **UEFI 控制台**：Mu-Silicium 的 `FrameBufferSerialPortLib` 是「把串口输出画到帧缓冲」，
  而 sprd UART 既不是 8250 也不是 PL011（Linux 用 `drivers/tty/serial/sprd_serial.c`，自带寄存器布局），
  所以要自己写一个 `SerialPortLib`（约 150 行：CTL0/CTL1/CTL2/STA/CLKDIV/FIFO 三件套）。
  这一步是「阶段 1 只做串口」的全部工作量，也是最值得先做的一步。

### 2.5 存储：XNU 侧完全空白

UniXNU 现有块设备只有 `NativeUfs`（UFS）与 `NativeVirtioBlock`（virtio），E5 是 SDHCI 类 eMMC
（`sprd,sdhci-r11`，HS400 enhanced-strobe + CQHCI + ICE），**vendor FDT 里没有 UFS 节点**。
需要两个驱动，模式可照 `NativeUfs` 抄：
1. UEFI 侧 `BlockIo`（读 payload 卷 / 读 ramdisk）；
2. XNU 侧 `IOService` + 块设备（给 `apfs` 挂 root）。
第 2 个可以推到 M5，前面阶段用 ramdisk（`osenvironment = "ramdisk"`）绕过去。

## 三、差距矩阵

| 能力 | E5 现状 | 可复用 | 量级 |
| --- | --- | --- | --- |
| 引导固件 | 只有 LK，无 UEFI；Mu-Silicium 无 Unisoc 包 | `Silicon/Silicium/SiliciumPkg`（Sec、内存、Fb、菜单） | 大：新 SoC 包 + 板级包 |
| 串口 | UART1 @0x20210000，115200n8，已由 LK 初始化 | 无（sprd 非 8250/PL011） | 小：SerialPortLib + XNU PE_putc |
| 中断控制器 | GICv3，GICD 128KiB + GICR 1MiB，stride 0x20000 | `ArmCpu` 原样 | 无 |
| 定时器 | armv8-timer，PPI 13/14/11/10，CNTFRQ 待测 | `ArmCpu` 原样 | 无 |
| SMP | PSCI 0.2 / SMC，8 核 | `ArmCpu` 原样 | 无（ADT 里写 `/psci`） |
| 设备树 | vendor FDT 285KB，可解析 | `ArmDTPlatform`；`scripts/qemu-entry/make-dt.py` 作模板 | 中：写 `make-e5-adt.py` |
| 内核载入 | 无 | `DarwinBoot` 原样 | 小 |
| 内存 | 2GiB，reserved-memory 有 CP 436MiB + CMA 360MiB | `RamManagerDxe` / `MemoryInitPeiLib` | 中：要给 XNU 划连续窗口 |
| 显示 | DPU + DSI + ST7365P 480x320 | `NativeMobileDisplay`（部分） | 大 |
| GPU | Mali-G57，"sprd,mali-natt" | **无**（Adreno 专用栈） | 极大，暂不排 |
| 触摸 | SPI1，节点写 novatek* | `NativeNovatekTouch` | 中 |
| 存储 | eMMC SDHCI + CQHCI + ICE，无 UFS | `NativeUfs` 仅作模式参考 | 大 |
| root 卷 | 无 | 需按 UniXNU-sheng 的方式拼 userdata | 中 |
| 电源/充电 | UMP9620 PMIC + AW32257 | 无 | 中 |

## 四、三条路线的取舍

**A. Mu-Silicium 式 EDK2 移植（推荐）**
新增 `Silicon/Unisoc/UMS9621Pkg`（SoC：内存初始化、sprd UART SerialPortLib、arch timer、
GIC 最小初始化、eMMC BlockIo）+ `Platforms/Unisoc/e5Pkg`（板级：DSC/FDF、FV、boot shim、
UniXNU 与 DarwinBoot 打进去）。
优点：UniXNU / DarwinBoot / ArmDTPlatform / ArmCpu 全部原样用，`build payload`、`bootimg` 流水线不用改。
缺点：EDK2 平台移植本身就是一大块工作（Mu-Silicium 每个 SoC 包都不小）。

**B. 自研裸机 loader（跳过 UEFI）**
利用 2.1 的结论：LK 把 payload 装到 0x80080000 并跳过去，我们在那里填 `boot_args`、自己解 Mach-O、
建页表、跳 `_start`。
优点：最快看到 `XNU:` 的第一行日志（M0 就能到）；不依赖 EDK2 的 DXE/BDS/GOP 全栈。
缺点：DarwinBoot 的逻辑（Mach-O 载入、page table、trust cache、ramdisk 定位）要重写一遍；
而且这些逻辑 UEFI 版已经有了，等于重造。**结论：只当 M0 验证手段，不做主路线。**

**C. 改 LK 自己当 loader**
LK 源码不在手上（`uboot_a/b` 是展锐定制二进制，release: user），闭源不可改。**排除。**

推荐：**先 B 后 A**。B 用最小代价把「LK 交接契约、CNTFRQ、MPIDR、GIC 探测、eMMC 探测」这些
未知项一次性测出来，再把结论喂给 A。

## 五、分阶段计划

### M0 裸机 stub：把「LK 交接契约」测实 —— **已完成，见 `docs/m0-bringup-report.md`**

> 实测结论：入口在 **EL2**、`x0` = `0x86000000` 的 vendor FDT（与 Android 在跑的一致）、
> 可以自己降到 EL1、GICv3 系统寄存器可用、4K 页粒度可用、PA 40 位、CNTFRQ 26MHz、
> PSCI 0.2 副核全 OFF（`CPU_ON` 首次尝试导致 LK panic 重启循环，暂缓）。
> 报告通道：ramoops console 区（`0xfffb0000`）+ `adb cat /sys/fs/pstore/console-ramoops-0`。

- 工具链：`clang -target aarch64-none-elf` + `ld.lld`，本机已验证可用（见六）。
- 打包：把 stub 当成 boot_b 的 kernel（沿用 `e5-linux/kernel/build-boot-image.py`），LK 会装到 0x80080000 并跳过去。
- 打印这些值，一次把未知项消掉：
  `CurrentEL`、`MPIDR_EL1`（8 核亲和值，ADT 的 `cpuN.reg` 要它）、`MIDR_EL1`、
  `CNTFRQ_EL0`（ADT 的 `timebase-frequency` 要它）、`x0`（确认是不是 0x86000000 的 DTB）、
  `GICD_TYPER` / `GICD_PIDR2`（确认 GICv3 与 SPI 数）、`GICR_TYPER`（确认 redistributor 数）、
  eMMC 控制器 `0x22200000` 的 SDHCI 版本/能力寄存器。
- 验收：UART1 @115200 上打出上述值。
- 前提：LK 已经把 UART1 的时钟与 pinctrl 配好了（它自己就在这个口上打日志），我们只写数据寄存器。

### M1 串口 UEFI（不做显示）

- `Silicon/Unisoc/UMS9621Pkg`：`SerialPortLib`（sprd UART1）、`TimerLib`（arch timer + CNTFRQ）、
  `DebugLib` 全指向串口、最小 GIC 初始化（UEFI 阶段基本不用中断，可以不做）。
- `Platforms/Unisoc/e5Pkg`：DSC/FDF，把 UniXNU 与 DarwinBoot 打进 FV，`[uefi_fd] base/size` 按 M0 实测的可用内存定。
- 验收：串口出现 EDK2 DEBUG 输出，进 BDS，出现 UniXNU 菜单（`Simple Text In` 走串口终端驱动）。

### M2 payload 卷

三条可选，按省事排序：
1. 把 payload FAT 镜像当 FV 里的 raw 文件，用 `FvSimpleFileSystemDxe` 暴露成只读卷 —— 不需要存储驱动，
   但**要确认 UniXNU 能找到这个卷**（它默认扫 SimpleFileSystem 句柄，理论上可行）【U】。
2. LK 已把 ramdisk 装在 0xc0000000（见 1.3），把它当内存盘。
3. 写 eMMC `BlockIo` + FAT —— 最正统，也最慢。

### M3 UniXNU + DarwinBoot

payload 里要有 `kernel.macho`、`kernel.macho.adt`、`unixnu.toml`、trust cache 与 ramdisk 占位。
验收：串口出现 `UNIXNU:` 前缀日志，接着 `DARWINBOOT:` 前缀日志，最后交出 EL1。

### M4 kernel entry

- 写 `make-e5-adt.py`（ADT 生成器）：`/chosen`、`/cpus`、`/arm-io/intc`、`/defaults`、`/options`、
  `/product`、`/buttons`，外加 memory-map 三个占位符与空 NVRAM proxy。
- XNU 侧：新增 sprd UART 的 `PE_putc`；确认 `board_config.h` 的 `MAX_CPUS`（BCM2837 是 4，我们要 8）、
  `MAX_CPU_CLUSTERS`（1 → 2）。
- 验收：XNU 打出 boot-args 与 platform expert 的日志，停在「找不到 root」或某个缺失驱动处 —— 这就是
  UniXNU bring-up 文档里「阶段 5 完成」的定义。

### M5 存储与 root

XNU 侧 SDHCI/eMMC 块驱动（模式照 `NativeUfs`）+ userdata 镜像（照 UniXNU-sheng 用 OS/SystemOS 两个 dmg 拼）。
验收：`apfs` 挂上 root，进入「阶段 6」。

### M6 显示 / 触摸 / 电源

DPU + DSI + ST7365P（`NativeMobileDisplay` 改）、SPI1 触摸（`NativeNovatekTouch` 改）、
UMP9620 PMIC + AW32257 充电。GPU 不在计划内。

## 六、本机工具链现状

- macOS 27.0.1（Darwin 27.0.0）。**没装 Xcode**（`xcodebuild` 报 requires Xcode），只有 Command Line Tools。
- 裸机交叉编译可用【C】：`clang -target aarch64-none-elf -c` 通过，`ld.lld -Ttext=0x80080000` 链接通过。
- `llvm-objcopy` / `llvm-readelf` / `llvm-objdump` 在 `/opt/homebrew/opt/llvm/bin`（Homebrew LLVM 23.1.1），
  **不在 PATH 里**，脚本里要显式写全路径。
- 没有 `dtc`，没有 `aarch64-linux-gnu-gcc`。
- Python 3.9（`/Library/Developer/CommandLineTools`），已装 `pyfdt`，用法 `from pyfdt.pyfdt import FdtBlobParse`；
  `dumps/live-fdt.dts` 就是它导出的。
- **XNU 内核这一步在本机做不了（已核实，2026-10-23）**。`UniXNU/scripts/unixnu/kernel.py:75` 里写死：
  `tool('xcodebuild', 'the kernel step builds with the Xcode iphoneos SDK')`，
  即 `./unixnu build kernel` 必须有 **Xcode 的 iphoneos SDK**。
  本机 `xcrun --sdk iphoneos --show-sdk-path` 直接报
  `xcrun: error: SDK "iphoneos" cannot be located`，只有 `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`。
- 磁盘也装不下 Xcode：`/Volumes/Projects` 只剩 2.4 GiB（98%），系统卷剩 23 GiB，而 Xcode 要 40 GiB 以上。
- 其余步骤的工具需求（读 `scripts/unixnu/*.py` 的 `tool()` 调用得到）：
  | 步骤 | 需要 | 本机 |
  |---|---|---|
  | `build kernel` | `xcodebuild` + iphoneos SDK | **缺** |
  | `build firmware` | `clang`、`python3`、`make` | 有 |
  | `build package` | `clang` | 有 |
  | `build payload` | `llvm-nm`、`mtools`、`hdiutil` | 有（mtools 待装） |
  | `bootimg` | `clang`、`llvm-objcopy`、`llvm-nm`、`fdtput`/`fdtget`(dtc) | dtc 待装 |
  | `ipsw fetch` 的 userdata / Data seed | `hdiutil`、`diskutil` | 有（macOS 自带） |
- 结论：**内核必须靠 GitHub Actions 的 macOS runner 出**（UniXNU 自带 `.github/workflows` 的
  `macOS build` workflow，公开仓库免额度）。本地能做的部分（firmware / package / bootimg /
  ADT / 裸机 loader）不受影响。

## 七、未验证项与风险

按「先测哪个能省最多工作」排序：

1. `CNTFRQ_EL0` 实际值【U】—— ADT 的 `timebase-frequency` 与 ArmCpu 都依赖它。M0 一条 `mrs` 解决。
2. `MPIDR_EL1` 亲和值与 8 个 GICR 的对应关系【U】—— 决定 `/cpus/cpuN.reg` 与 IPI 映射。M0 解决。
3. LK 是否接受任意 raw payload【U】—— 日志里每次都打 `The kernel is in raw format`，但样本全是 Image。
   M0 直接拿 stub 试；失败就退回 hardware 文档里的 wrap-image 方案（stub 前面垫一个合法 ARM64 Image 头）。
4. boot_b 64MiB 分区能放多大的 kernel【U】—— XNU 内核（未压缩）+ trust cache 要算清楚。
5. eMMC 控制器能力寄存器（SDHCI 版本、是否强制 CQHCI）【U】—— 决定 UEFI/XNU 驱动怎么写。
6. UEFI 自己初始化 UART1 时的时钟与 pinctrl 需求【U】—— LK 已配好，但 EDK2 从 FV 起来时这些配置还在。
7. Mu-Silicium 是否已有非 PCI 的 SDHCI 驱动可抄【U】—— `Silicon/*` 下没看到 SdMmc 目录，大概率要自己写。
8. UniXNU 能否识别 `FvSimpleFileSystemDxe` 暴露的卷【U】—— 决定 M2 走哪条路。
9. reserved-memory 里 CP 436MiB + CMA 360MiB 是否真的不可用【U】—— 影响 ADT 的 `dram-size`。
10. 触摸 IC 实际型号【U】；LK 是否已初始化 DPU/DSI（开机 logo）【U】。

## 八、参考

- 本仓库：`docs/research-e5-hardware.md`、`dumps/live-fdt.dts`、`dumps/uboot_log.bin`
- UniXNU：<https://github.com/UniXNU/UniXNU>，文档 <https://unixnu.com>，
  `docs/porting/{bring-up,device-tree,device-repository,uefi-firmware}.md`，设备仓 `UniXNU/UniXNU-sheng`
- Mu-Silicium：<https://github.com/Project-Silicium/Mu-Silicium>
- XNU 上游：`xnu-10063.141.1`（`osfmk/arm64/start.s`、`pexpert/pexpert/arm64/{boot.h,board_config.h,BCM2837.h}`）
- 邻仓：`/Volumes/Projects/e5-linux`（LK 打包脚本、wrap-image、主线内核）
