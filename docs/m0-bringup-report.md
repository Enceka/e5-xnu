# M0 实测报告：在荣悦 E5 上跑起自己的裸机代码

> 目标：在研究文档假设的基础上，用一段 18KB 的裸机 stub 实测「LK 交接给我们的是什么」，
> 把 XNU 移植的入口契约从【I·推断】升级成【C·确认】。
> 结论：**契约成立，且比预想的更好**——LK 把控制权交在 **EL2**，我们可以自己搭出 XNU 需要的 EL1 环境。

## 摘要

| 问题 | 答案 | 证据 |
|---|---|---|
| LK 交接时的异常级别 | **EL2**（不是 Linux 文档假设的 EL1） | `CurrentEL = 0x8` |
| `x0` 是什么 | 物理地址 `0x86000000` 的 **FDT**，与 Android 在跑的 vendor FDT 逐字节一致 | `magic 0xd00dfeed`、`totalsize 0x45c7c` |
| 能不能降到 EL1 | **能**，`HCR_EL2.RW=1` + `eret` 后 `CurrentEL = 0x4` | 自写 `drop_to_el1` |
| GICv3 系统寄存器接口 | 有（`ID_AA64PFR0.GIC = 1`） | `PFR0 = 0x11112222` |
| 4KiB 页粒度 | 支持（XNU arm64 默认 4K/16K 都可，本机只能 4K） | `MMFR0.TGran4 = 0`、`TGran16 = 1` |
| 物理地址位宽 | 40 位（1TiB） | `MMFR0.PARange = 2` |
| PSCI | 0.2，副核全部 OFF | `PSCI_VERSION = 0x10000` |
| 计数器频率 | 26MHz，与 sprd timer 一致 | `CNTFRQ_EL0 = 26000000` |
| UART1 能否发字节 | 能，FIFO 确实在排空 | `LSR = 0x1b63`、2536 字节全被接收 |
| `lcd_base=0xbe000000` 是否可写 | **可写**，读回 `0xa5a5a5a5` | `dumps/m0-report-fb.txt` |
| PSCI `SYSTEM_RESET` 能否复位 | **不能**，SMC 不返回（挂死） | 同上，见第十一节 |
| 副核能否拉起 | **未解决**，第一次 `CPU_ON` 就把系统打进 LK panic 重启循环 | 见第五节 |

## 一、实验装置

- 代码：`boot/m0/`（`entry.S` + `m0.c` + `m0.ld` + `build.sh`），产物 `m0.bin` 18024 字节。
- 位置：LK 把 boot 分区里的 kernel 固定装到 **`0x80080000`** 并从那里跳转，所以 stub 的链接地址就是 `0x80080000`，
  `.text.header` 必须是合法的 arm64 Image 头（`b _entry` + `text_offset 0x80000` + `image_size` + `ARMd` magic）。
- 刷入：`dd of=/dev/block/by-name/boot_b bs=4096 seek=1 conv=notrunc`（boot.img 的 kernel 从 0x1000 开始）。
  配合 `misc` 里 slot_b 记录（prio 15 / tries 2）让 LK 去启动 slot_b。
- 恢复：LK 每启动一次就把 `tries_remaining` 减一并写回 misc，耗尽后自动回滚 slot_a（Android）。**不需要手动刷回**。
  一次「刷入 → 重启 → Android 起来 → adb 可取」的周期约 8–10 分钟。
- 取报告：**ramoops console 区**（`0xfffb0000`，12 字节头 `sig 0x43474244` + payload），
  重启进 Android 后 `adb exec-out "cat /sys/fs/pstore/console-ramoops-0"`。
  每写一行就 publish 一次，所以即使中途崩了也能拿到崩之前的部分。
- 兜底调试通道：**LK 自己的 die handler**。`VBAR_EL2 = 0xbf001000`（LK 的向量表）在交接时没有被改，
  我们的代码一旦异常就会落进 LK 的 handler，它会把 ESR / ELR / iframe 打进 `uboot_log` 分区（16MiB 环形日志）。
  本机没有物理串口探针，这是唯一能看到寄存器现场的办法。

## 二、交接现场（`dumps/m0-report.txt`、`m0-report2.txt`、`m0-report3.txt`）

```
CurrentEL = 0x8                          <- EL2
MPIDR_EL1 = 0x81000000                   <- aff0=0 aff1=0 aff2=0，bit24 MT=1
MIDR_EL1  = 0x412fd050                   <- ARM Cortex-A55 r0p2
SCTLR_EL1 = 0x30d00800                   <- MMU / D-cache / I-cache 全关
DAIF      = 0x3c0                        <- 全部屏蔽
VBAR_EL1  = 0x0                          <- 未设，此时 EL1 异常会跳到地址 0
SCTLR_EL2 = 0x30c50830                   <- MMU 关
HCR_EL2   = 0x122                        <- RW=0：EL1 当前是 AArch32
VBAR_EL2  = 0xbf001000                   <- LK 自己的向量表
CPTR_EL2  = 0x33ff                       <- TFP=0，FP/SIMD 不 trap
ID_AA64PFR0_EL1  = 0x11112222            <- EL0/1/2/3 全 AArch64，FP+AdvSIMD，GIC sysreg
ID_AA64MMFR0_EL1 = 0x101122              <- PARange=2(40bit) ASID=16bit 4K✓ 16K✗ 64K✓
CNTFRQ_EL0 = 26000000
x0 = 0x86000000  x1 = x2 = x3 = 0
```

**这是本轮最重要的发现**：`HCR_EL2.RW = 0` 意味着此刻 EL1 处于 **AArch32** 状态。
如果直接 `eret` 到 EL1 去跑 XNU（AArch64 代码），CPU 会把指令当 AArch32 解释。
所以移植的第一步不是别的，而是：**先 `msr hcr_el2, #(1<<31)`，再 `eret`**。`drop_to_el1` 已实测走通：

```
msr   sp_el1, x1          // 给 EL1 一个栈
msr   vbar_el1, x2        // 给 EL1 一张向量表
msr   elr_el2, x0
mov   x9, #0x3c5          // EL1h, DAIF 全屏蔽
msr   spsr_el2, x9
mov   x9, #1
lsl   x9, x9, #31         // HCR_EL2.RW = 1（同时清 TGE/IMO/FMO/AMO）
msr   hcr_el2, x9
msr   cptr_el2, xzr
isb
eret
```

## 三、FDT

`x0` 指向的 FDT 与 `dumps/live-fdt.dtb`（Android 的 `/sys/firmware/fdt`）**完全一致**：
`magic 0xd00dfeed`、`totalsize 0x45c7c`(=285820)、`off_dt_struct 0x48`、`off_dt_strings 0x3ac40`、
`version 0x11`、`last_comp_version 0x10`、`boot_cpuid_phys 0`、`size_dt_strings 0x51e1`、`size_dt_struct 0x3abf8`。

-> **不用自己拼 DTB，也不用担心 dtbo 没合并**：LK 传进来的就是合并好 dtbo 的那棵 vendor FDT【C】。

## 四、GIC（`GIC-600`，两组寄存器都在）

```
GICD_CTLR  = 0x10        <- ARE 已开，所有 group 未使能（交给 OS）
GICD_TYPER = 0x790408    <- 288 条 SPI、ESPI=1、LPI=1、SecurityExtn=1
GICD_IIDR  = 0x201743b
GICD_PIDR2 = 0x0         <- arch rev 报 0，异常（GICv3 本应为 3），但 ARE/GICR 都正常
```

8 个 redistributor（stride `0x20000`，基址 `0x12040000`），`ctlr = 0`、`waker = 0`、`prop = 0`、`pend = 0`：

| 帧 | GICR_TYPER | affinity（高 32 位） |
|---|---|---|
| 0 | `0x0000000000000020` | 0 |
| 1 | `0x0000000100000120` | 1 |
| 2 | `0x0000000200000220` | 2 |
| 3 | `0x0000000300000320` | 3 |
| 4 | `0x0000000400000420` | 4 |
| 5 | `0x0000000500000520` | 5 |
| 6 | `0x0000000600000620` | 6 |
| 7 | `0x0000000700000730` | 7 |

两个必须记住的坑：

1. **`GICR_TYPER.Last` 位全为 0**。Linux 的 `gic_redist_init` 靠 Last 位遍历，在这颗芯片上会走飞。
   驱动必须按 FDT 的 `#redistributor-regions = 1` + CPU 数（8）硬编码。
2. **第 8 帧之后读到的是 GICD 的镜像**：`0x12040000 + 8*0x20000 = 0x12140000` 读回来的
   `typer = 0x201743b`、`iidr = 0x790408` 正是 GICD_IIDR / GICD_TYPER 的值。所以确实只有 8 个 redistributor。

## 五、PSCI 与 SMP（未解决）

```
PSCI_VERSION = 0x10000            <- PSCI 0.2，与 DT 的 "arm,psci-0.2" 一致
AFFINITY_INFO: 0x0 -> 0 (ON，就是自己)
               0x1 0x2 0x3 0x4 0x5 -> 1 (OFF)
               0x100 0x101 -> 1 (OFF)
```

试过对 affinity `{0x1,0x2,0x3,0x4,0x5,0x100,0x101}` 逐个 `CPU_ON(target, sec_entry, 0)`，
每个副核有自己的 4KiB 栈、把 `mpidr/el/midr/cntfrq/gicd` 写进自己的 buffer、置完成标志后 `CPU_OFF`。

结果：**第一条 `CPU_ON` 之后什么都没有**，连「返回码」那一行都没印出来，随后系统进入
`bootloader panic, save log ...` 的**重启循环**（`uboot_log4.bin` 里 3062 次同样的
`data fault: Read access from PC 0xbf020200, FAR 0xdeaddeaddeaddeb5, iss 0x0 (DFSC 0x0)`
——PC 在 LK 自己的代码段、FAR 是 `0xdead...` 毒值，说明是 LK 的 panic 保存路径自己在踩毒指针递归崩溃，
真正的第一现场被淹掉了）。

最可能的根因（**未验证**）：副核被 PSCI 拉起时落在 EL1，而 `HCR_EL2.RW = 0` -> EL1 是 AArch32，
AArch64 的 `sec_entry` 被当 AArch32 解释。下次试验应该在 `CPU_ON` 之前先置 `HCR_EL2.RW = 1`，
或者在 `sec_entry` 开头同时兼容两种状态。

代价评估：一次失败会连带 LK panic 重启循环，一个周期 10 分钟，且拿不到干净现场。
**SMP 探针暂缓**，不阻塞主线（XNU 的副核入口 `start_cpu` 本来就是 EL1 代码，等 M4 有串口日志了再一起做）。

## 六、存储

`sdhci@22200000`（eMMC）：

```
HOST_VERSION   = 0x4          <- SD Host Spec v4
CAPABILITIES   = 0x1e6d0080   <- 8-bit ✓ ADMA2 ✓ SDMA ✓ high-speed ✓ VDD 3.0V ✓ VDD 1.8V ✓
                                 VDD 3.3V ✗ 64-bit ✓ 64-bit_v4 ✓ base clock 字段 = 0（要从 DT 取）
CAPABILITIES_1 = 0x08000007   <- SDR50 ✓ SDR104 ✓ DDR50 ✓ ADMA3 ✓ HS400 位 ✗
```

**HS400 位为 0 不是问题**：LK 日志明确用 `8-bit HS400 enhanced-strobe + CQHCI + ICE` 跑这颗 eMMC，
HS400 的能力位在展锐的私有寄存器区（`0x100+`），不在标准 SDHCI caps 里。
UEFI 的 BlockIo 驱动只需要 SDMA/ADMA2 + 4-bit 就能读分区表，HS400 是性能问题不是功能问题。

## 七、UART1

```
LSR = 0x1b63 (TX_OVER 置位)   STS1 = 0xe0000   accepted = 2536 字节   still draining = 1
```

`0x20210000` 的 TX FIFO 确实在排空，说明 UART 已上电、时钟在跑、波特率分频可用。
**没有物理探针看不到输出**，但这条路是通的——焊 UART 是可选加速项，不是阻塞项。

## 八、对移植计划的影响（修正 `research-xnu-port.md`）

1. §2.1 写的「EL1 进入」不准确：**入口是 EL2，必须自己降到 EL1**（已给出可用代码）。
2. `MAX_CPUS 4 -> 8`、`MAX_CPU_CLUSTERS 1 -> 2` 的改动依然需要，但本机 8 个核的 MPIDR 亲和值
   到底是 `0x0..0x7` 还是 `0x0..0x5 + 0x100 + 0x101`，要等副核起来才能定论（FDT 的
   `cpu@0 cpu@100 ... cpu@700` 与 PSCI `AFFINITY_INFO` 的反馈互相矛盾）。
3. 好消息：`GIC sysreg = 1`，`ArmCpu` 里的 GICv3 系统寄存器路径（`ICC_*_EL1`）可以直接用，不用碰 MMIO 的 ICC 窗口。
4. 4KiB 页粒度可用、16KiB 不可用 -> XNU 的 `ARM64_PMAP_SUBPAGE_L1` / 页表配置必须走 4K 分支。

## 九、未解问题

- 副核的 MPIDR 亲和值到底是哪一套？PSCI 拉起后落在 EL1 还是 EL2？（阻塞 M4，不阻塞 M1–M3）
- `GICD_PIDR2.arch rev = 0` 是展锐定制还是读错了？影响：GICv3 vs GIC-600 的 LPI 属性处理。
- `GICR_TYPER` 低 32 位 `0x20/0x120/.../0x730` 的编码含义（`Processor_Number` 字段与 affinity 不自洽）。

## 十、复现

```bash
cd boot/m0 && ./build.sh                       # 产出 m0.bin
adb push m0.bin /data/local/tmp/ && adb shell 'dd if=/data/local/tmp/m0.bin of=/dev/block/by-name/boot_b bs=4096 seek=1 conv=notrunc'
# 写入 misc 的 slot_b 记录（见 docs/research-xnu-port.md 与 dumps/backup/misc-slot-b-trial.bin）
adb reboot
# 约 8-10 分钟后设备回滚到 Android
adb root && adb exec-out "cat /sys/fs/pstore/console-ramoops-0"
```

## 十一、帧缓冲与复位路径（第二轮实测）

### 11.1 `0xbe000000` 帧缓冲可写

`probe_fb()` 先读-改-写首字再恢复，确认这块内存在 LK 交接后仍然可访问：

```
framebuffer 0xbe000000 (lcd_base from cmdline):
    first word before = 0xffdeffde
    read back after write = 0xa5a5a5a5
    writable = 0x1
  painted magenta + 8 checker squares, holding 120s
  hold done
```

`0xffdeffde` 是 LK 走后留在缓冲里的内容。随后整屏刷品红 `0xffff00ff` 并在左上角画 8 个黑白格子，用 `CNTVCT_EL0` 忙等 120 秒。
**「写进去」已经证实；「有没有真的被 DPU 扫描出来」要靠人眼看面板**——这是 M1 能否直接复用 LK 留下的 framebuffer 当控制台的关键，也是本项目唯一可能的人眼可见输出（UART1 没有物理探针）。

### 11.2 复位路径全部不可用，只能等 LK 自己的看门狗

试过两条快路径，都失败：

1. **PSCI `SYSTEM_RESET`（`0x84000009`）**：SMC 不返回，CPU 直接挂死。报告能跑到
   `===== M0-E5-EL1-DONE =====` 和 `reset attempts:`，然后没有下文。
2. **AP 看门狗（`sprd,wdt-r2p0-fiq`，`0x641e0000`）**：连**读**都会挂死总线。
   DT 里 `aon` 的 `ranges = <0 0 0x64000000 0x01016000>`，`watchdog@1e0000` 落在这个窗口里，
   但 `probe_wdt()` 一读 `0x641e0000` 整机就停住，报告在 PSCI 一节之后直接截断
   （`dumps/m0-report-wdt.txt`，3224B）。原因是 AON 这组外设的时钟没开，
   访问被门控的寄存器会**卡住 AXI 总线**而不是产生一个可以处理的异常。

结论：**M0 阶段没有快速复位手段**，每轮实验结束后由 LK 自己的看门狗在约 5.5 分钟后复位回 Android。
一次「刷入 → 重启 → 取报告」约 6 分钟，这决定了后续迭代节奏。
若将来要提速，正确顺序是先通过时钟控制器打开 AON 的 `enable` 时钟再碰 `0x641e0000`，
而不是直接读写。

### 11.3 交付的可复用工具

- `tools/e5-misc.py`：ABC 槽位记录编解码，能逐字节复现原厂与 Linux 试验的两条真实记录。
- `tools/e5-cycle.sh flash|collect`：一轮迭代，刻意拆成两半以避开 5 分钟的工具超时；
  写入 `boot_b` 用 `dd bs=4096 seek=1`（kernel 区从 `0x1000` 开始）。
- `boot/m0/m0.c` 的编译期开关：`-DM0_FB` 打开帧缓冲探测，`-DM0_SMP` 打开副核探测（默认都关）。
