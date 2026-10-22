# E5 UEFI 移植进展（M1）

把 EDK2 + Project Silicium 拉到荣悦 E5（Unisoc UMS9621 / Qogirn6lite）上的实况记录。
路线 A 的依据见 `docs/research-xnu-port.md`：XNU 需要有人填好 `boot_args` 并跳进去，
而 UniXNU 生态里的 DarwinBoot、ArmCpu、ArmDTPlatform 都建立在 UEFI 之上，所以先把
UEFI 做出来。

## 一、引导链

E5 没有 UEFI 固件，入口在 LK：

```
bootrom -> FDL -> uboot 分区 (LK) -> [boot_a/boot_b 偏移 0x1000] -> 0x80080000
```

LK 永远把 boot 分区里 offset `0x1000` 的内容装到 **0x80080000** 并跳过去，`x0` 是合并后的
vendor FDT 物理地址（M0 实测：`x0 = 0x86000000`）。所以固件镜像必须是"能被装到
0x80080000 并跳转"的东西，而 EDK2 的 FD 要落在 `PcdFdBaseAddress`，两者之间需要一个
搬运器：

```
0x80080000  BootShim（合法 AArch64 Image 头 + 拷贝循环 + 看门狗布置）
0x80080140  FD 的副本（作为 shim 的 payload 直接拼在文件尾部）
      |
      +-- 运行时拷到 0x80800000，然后 br 过去
```

镜像 = `BootShim.bin` + `SILICIUM_UEFI.fd` 纯拼接，和 Project Silicium 的
`build_uefi.py:251` 完全一致（`boot_shim_payload.read_bytes() + uefi_fd.read_bytes()`），
没有任何头部封装，只把总长度回填到 Image 头偏移 16 的 `image_size`。

### 为什么 FD_BASE 是 0x80800000 而不是 0x80200000

shim 用 `ldp/stp` 前向 16 字节拷贝。如果目标 `0x80200000` 落在源区间
`0x80080000..0x80480000` 里，拷贝会在读到之前就覆盖源数据。4 MiB 的 FD 必须放到
源区间之外，于是 `FD_BASE = 0x80800000`。

## 二、包结构

```
uefi/
  BootShim/BootShim.S            链接到 0x80080000，含 Image 头与看门狗布置
  BootShim/BootShim.ld
  build-e5.sh                    build + 编 shim + 拼接，产物在 uefi/build/
  tools/mk-boot-payload.py       校验 ARMd magic、回填 image_size、拼接
  Platforms/Unisoc/e5Pkg/
    e5.dsc                       平台描述，USE_CUSTOM_DISPLAY_DRIVER=1
    e5.fdf                       FD/FV 布局
    Include/{APRIORI,DXE}.inc    组件清单
    Library/MemoryMapLib/        整机内存图（也是 "Display Reserved" 的来源）
  Silicon/Unisoc/UMS9621Pkg/
    UMS9621Pkg.dec/.dsc.inc      SoC 层：GIC 地址、8 核 2 cluster、定时器 PPI
    Include/Ums9621.h            所有基址与寄存器偏移的单一出处
    Library/SprdDebugSerialPortLib/  UART1 + ramoops console 双通道
    Drivers/SprdGopDxe/          自写 GOP（面板只有 320x480）
    Drivers/E5BringupDxe/        派发结束时的探针 + 主动复位
```

Silicium 自带的 SEC、DXE core、BDS、DtPlatformDxe、ArmGicDxe、TimerDxe、CpuDxe 全部原样
复用；我们只补 SoC 与平台两层，和 stonePkg / j706fPkg 的分层方式一致。

### 面板 320x480：必须自写 GOP

`SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c:104-125` 硬性要求分辨率不低于 640x475，否则
直接 `EFI_UNSUPPORTED`。E5 是 320x480 竖屏（DPU `PANEL_SIZE = 0x01e00140` = `480<<16|320`），
所以 `e5.dsc` 里 `USE_CUSTOM_DISPLAY_DRIVER = 1`，走 `SprdGopDxe`：
它按名字 `"Display Reserved"` 从 `MemoryMapLib` 拿到 `0xbe000000`（长度 `0x9e4000`），
用 `PcdFrameBufferWidth/Height/ColorDepth` = 320/480/16 填 mode，16bpp 走
`PixelBitMask`（R `0x0000F800` / G `0x000007E0` / B `0x0000001F`），Blt 包一层
`RaiseTPL(TPL_NOTIFY)` 调 `FrameBufferBlt`。

## 三、必须覆盖的 PCD（都是踩出来的）

| PCD | 值 | 为什么 |
| --- | --- | --- |
| `PcdArmArchTimerSecIntrNum` | 13 | vendor FDT `arm,armv8-timer` 是 13/14/11/10；ArmPkg 默认 29/30/27/26 |
| `PcdArmArchTimerIntrNum` | 14 | 同上 |
| `PcdArmArchTimerVirtIntrNum` | 11 | 同上 |
| `PcdArmArchTimerHypIntrNum` | 10 | 同上 |
| `PcdCoreCount` | 8 | 6×A55 + 2×A76 |
| `PcdClusterCount` | 2 | |
| `PcdGicDistributorBase` | 0x12000000 | |
| `PcdGicRedistributorsBase` | 0x12040000 | 8 帧，stride 0x20000 |
| `PcdSystemMemoryBase` | 0x80000000 | 2 GiB |
| `PcdFrameBufferWidth/Height/ColorDepth` | 320/480/16 | |

定时器 PPI 错的话，`ArmPkg/Drivers/TimerDxe/TimerDxe.c:394` 会去 arm 一个永远不触发的
PPI，DXE 在第一个 timer 事件上永久停住 —— 一个没有任何输出的死法。

CLANGDWARF 路线还要自己补 `-D CLANG -D ENABLE_SECUREBOOT=0`（`SiliciumPkg.dsc.inc:19-20`
只给 CLANGPDB 定义了这两个宏），以及 `-fno-stack-protector`（EDK2 不提供
`__stack_chk_guard` / `__stack_chk_fail`）。

## 四、构建

依赖仓（都放在 `/tmp`，GitHub 直连不通时用 `https://gh-proxy.com/https://github.com/...`）：

```
/tmp/mu_basecore     microsoft/mu_basecore  branch release/202608（+5 个手工补的子模块）
/tmp/mu_plus         microsoft/mu_plus      branch release/202608
/tmp/mu_oem_sample   microsoft/mu_oem_sample
/tmp/uefi-sheng      UniXNU/uefi-sheng      只用到 Silicon/Silicium
```

`/tmp/e5uefi/env.sh` 设 `WORKSPACE`、`EDK_TOOLS_PATH`、`PACKAGES_PATH`（必须包含平台与
Silicon 目录，否则 `build` 直接 `- Failed -` 且**不打印任何错误**）、`CLANGDWARF_BIN`、
`CLANG_BIN` 指向 `/opt/homebrew/opt/llvm/bin/`。

macOS 注意：
- `build` 的 shebang 是 `env python`，本机没有 `python`；要放一个 `#!/bin/sh` 加
  `exec /usr/bin/python3 "$@"` 的包装脚本（直接软链会触发
  `xcode-select: Failed to locate 'python'`）。
- `ld.lld` 在 `/opt/homebrew/bin/ld.lld`，**不在** `/opt/homebrew/opt/llvm/bin/`。
- 链接脚本里不能用 `//` 注释，只能 `/* */`。

```
sh uefi/build-e5.sh DEBUG      # 产物 uefi/build/{SILICIUM_UEFI.fd,BootShim.bin,e5-uefi.bin}
```

`FD_BASE` / `FD_SIZE` / `FD_BLOCKS` 是 fdf 的宏，必须由命令行 `-D` 传入（Silicium 的
stuart 流程 `build_uefi.py:228` 会注入；裸调 `build` 不给就报
`expected Hex base address near line 22`）。

## 五、迭代回路

```
sh tools/e5-uefi-cycle.sh flash     # 编 + 写 boot_b + 改 misc 让 LK 选 slot_b
sh tools/e5-uefi-cycle.sh collect   # 等 Android 回来 + 取 ramoops console
```

`flash` 的要点：
- payload 写到 `boot_b` 的 `bs=4096 seek=1`（跳过 boot.img 头，对应 LK 的 `kernel offset 0x1000`）。
- 回填 boot.img 头的 `kernel_size`（偏移 8），否则 LK 按旧长度读。
- misc 的 ABC 记录必须是 **A prio 14 / tries 1 / succ 1，B prio 15 / tries 2 / succ 0**：
  B 的 priority 必须**严格大于** A（相等不切换）；`tries_remaining` 写 1 会被 LK 当成
  "这是最后一次"而直接进 download 模式，可用值必须不小于 2。

调试通道只有一条：`ramoops` console 区（`0xFFFB0000`，12 字节头 + payload），
因为板子上 UART1 没有可焊的测试点，而 FD 里没有 USB 栈。`collect` 从
`/sys/fs/pstore/console-ramoops-0` 把它读出来。

## 六、安全网：两次挂死的代价

第一次实机（`dumps/uefi-report-014501.txt`）是好的：

```
Project Silicium for Rongyue E5
Firmware Version 4.0 Build at 17:36:48 on Oct 22 2026
MMU In
MMU: 39 regions
MMU: map valid
MMU: HOBs built, enabling
UpdateRegionMapping RegionStart: 0x30120000 or RegionLength: 0x100 are not page aligned!
MMU: enabled
Failed to Configure MMU! Status = Invalid Parameter
```

也就是说 BootShim 搬运、SEC 入口、串口/ramoops 输出、MemoryMapLib 全都通了，只卡在一个
4 KiB 对齐问题上（`{"DPU GLB", 0x30120000, 0x100}` 长度不是页的整数倍，改成 `0x1000`）。

修完之后刷进去，**整机失联 20 分钟以上**，USB 上连设备节点都没有。教训是：

> LK 的 panic handler 之所以能把机器救回来，是因为我们的异常落进了它自己的 EL2 向量表。
> 一旦卡在一个不发异常的循环里（比如等一个永不置位的寄存器），就永远不会有复位。
> M0 阶段能靠 LK 的看门狗兜底，是因为 M0 会 `wfe` 停住；换成固件之后这个假设不再成立。

所以现在有两层安全网。

### 6.1 BootShim 里布 AP 看门狗（60 秒）

难点是 AON 那组外设**默认被时钟门控**，而被门控的寄存器一读就完蛋。M0 阶段读
`0x641e0000` 当场把机器读死，当时记成「AXI 总线卡死」；后来从 `uboot_log` 里翻到原始现场，
其实是**同步外部异常**被 LK 的 EL2 向量接住了：

```
data fault: Read access from PC 0x800833f4, FAR 0x641e0020, iss 0x10
ESR 0x96000010: ec 0x25, il 0x2000000, iss 0x10, EL2
Synchronous External abort, not on translation table walk
```

所以那次「挂死」只是异常之后 LK 的 panic 路径自己崩掉，不是总线真的锁死。不过结论不变：
**碰之前先开时钟**。两个门都在
`syscon@64900000` 的 `aonapb-gate`（`compatible = "sprd,ums9621-aon-gate"`）里，
是 set/clear 型门（`clk_sc_gate_toggle()`：使能 = 往 `reg + sc_offset` 写 mask）：

| 时钟 | 定义 | 使能写 |
| --- | --- | --- |
| `ap-wdg-eb` | `reg 0x8, sc_offset 0x1000, BIT(29)` | `0x64901008 = 0x20000000` |
| `ap-wdg-rtc-eb` | `reg 0x18, sc_offset 0x1000, BIT(7)` | `0x64901018 = 0x00000080` |

然后按 `sprd_wdt.c` 的顺序布置：解锁 `0x641e0020 = 0xe551`、清 `INT_CLR 0x0c = 0x9`、
`LOAD_HIGH 0x04 = 0x1e`（60 秒，一 tick = 1/32768 s，所以 `60*32768 = 0x1e0000`）、
`LOAD_LOW 0x00 = 0`、`CTRL 0x08 = 0xf`（`NEW_VER_EN|INT_EN|CNT_EN|RST_EN`）、
再写 `0x20 = 0` 锁回去。`-DNO_WATCHDOG` 可关。

**已知缺陷：这个 60 秒并没有生效。** 实测从死到复位是 4 分 51 秒，正好是 LK 自己预先布好的
约 300 秒超时。原因是 `sprd_wdt_load_value()` 在写 `LOAD_HIGH/LOAD_LOW` 之前必须先轮询
`INT_RAW & LD_BUSY`（`SPRD_WDT_INT_RAW` 的 bit4）等它清零，直接写的话 LOAD 值会被丢弃，
只有 `CTRL` 里的使能位生效。也就是说当前实际起作用的是 LK 留下的那个 5 分钟死手开关，
我们自己的 60 秒要等这段轮询补上。复位确实发生了，`uboot_log` 里下一次启动的现场是：

```
hw_rst_mode==2                    (adb reboot 正常是 2002)
reg_rst_mode is 0
hw watchdog int raw status 0x9     (bit0 INT_RAW + bit3 RST_RAW)
reboot_mode_check rst_mode=0x2
```

### 6.2 E5BringupDxe：走到 DXE 末尾就自己复位

第二层是主动退出。`E5BringupDxe` 在 `gEfiEndOfDxeEventGroupGuid` 上挂回调，打印
GOP mode、配置表项数、完整内存图（类型/起止/属性），然后调 `gRT->ResetSystem`。
`ResetSystemRuntimeDxe` + `ArmPsciResetSystemLib` 在 FD 里，所以这是 PSCI `SYSTEM_RESET`。

这一层解决的是另一类失败：固件其实跑得很好，落进 BDS 的前端页面等按键 —— 没有 USB 栈、
没有键盘，那就是永久停住，和挂死无法区分。行为由
`gUms9621PkgTokenSpaceGuid.PcdE5ResetAtEndOfDxe`（默认 TRUE）控制。

复位之后 LK 会消耗一次 `tries_remaining`，耗尽后自动回滚 slot_a 进 Android，日志就能读了。
整个过程两三分钟，不需要人碰机器。

## 七、payload 卷与 initrd（M2/M3 的地基）

UniXNU 的 DarwinBoot 不从内存指针读内核，它从**一个 FAT 卷**里按路径读：内核 Mach-O、
同目录的 `<kernel>.adt`、trust cache、Data 镜像。这个 FAT 卷就是 Android boot image 的
ramdisk 字段（`unixnu bootimg` 用 mtools 造好放进去）。所以固件必须把 ramdisk 变成一个
UEFI 块设备，`Fat` 驱动才能把它挂成卷。

LK 已经把答案写进设备树了。实机 `/chosen` 里有：

```
linux,initrd-start = <0xc0000000>
linux,initrd-end   = <0xc3640644>
```

即 **LK 固定把 ramdisk 放在 0xC0000000**（那次是 56.8 MiB 的 Android vendor ramdisk）。
`0xC0000000` 不与任何 reserved-memory 节点冲突（cp-modem 止于 0xA2400000，
nrphy-iqmem 止于 0xA5000000，logobuffer 在 0xBE000000）。

实现：`Platforms/Unisoc/e5Pkg/Drivers/E5PayloadDxe/`，照抄 sheng 的
`ShengBootMemoryDxe` 里 `PublishInitrd` 的做法——从 SEC 留下的
`gSiliciumEntryArgHobGuid` HOB 取出 x0 里的 FDT 指针，读 `/chosen` 的两个属性，
装一个只读 `EFI_BLOCK_IO_PROTOCOL`（512 字节块，`LastBlock = size/512 - 1`）。

**当前状态：编进 `[Components]` 以保证持续可编译，但刻意没有写进 `e5.fdf`。**
原因很直接——它在 DXE 派发期间跑，一旦出错就会盖掉 `E5BringupDxe` 打的
EndOfDxe 标记，那样下一轮日志又变成一片空白。等 DXE 确认跑通再挂进 FV。

启用时要同步做两件事，否则会 translation fault：

1. `MemoryMapLib.c` 加一条 `{"Initrd", 0xC0000000, 0x08000000, AddMem, MEM_RES, UNCACHED_UNBUFFERED_XN, Reserv, UNCACHED_UNBUFFERED_XN}`
   —— 0xC0000000 现在不在 39 个 region 里，MMU 没有映射它，读它就 fault；
2. 上面这条只是**预留窗口**，真实大小仍从设备树读，`E5PayloadDxe` 不认任何硬编码长度。

## 八、构建不是可重现的（无害，但要知道）

同一份源码连编两次，`e5-uefi.bin` 字节不同（第一个差异在 0x21A6，落在压缩后的
FVMAIN_COMPACT 里）。原因是 `SiliciumPkg/Sec/Sec.c:84` 把编译时刻写进了固件：

```c
DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n",
        PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
```

所以**不要用二进制比对来判断「代码有没有变」**——它永远会报不同。要比就比源码，
或者看 `Build/` 下各模块的 `.efi`。

## 九、屏显：唯一不用重启就能读的通道

### 9.1 为什么必须做

挂死的时候 ramoops 读不到东西。最直接的一次证据：一次整机挂死 20 分钟后回到 Android，
`/sys/fs/pstore/console-ramoops-0` 里还是**上一轮 Android 的内核日志**（最后一行
`reboot: Restarting system with command 'shell'`），我们的 `SprdDebugSerialPortLib`
一个字节都没写进去 —— 也就是 SEC 从来没跑到 `SerialPortInitialize`。

UART1 在物理上是有输出的（M0 阶段量过 TX FIFO 在排空），但没有探针；`uboot_log` 只能
事后读、而且要等回滚。所以面板是唯一「当下就能看」的通道。

### 9.2 BootShim 的阶段色

`BootShim.S` 在每一步刷**整屏**（`320*480 = 153600` 个 RGB565 像素），最后留在屏幕上的
颜色就是走到的最远阶段：

| 颜色 | 值 | 含义 |
| --- | --- | --- |
| 蓝 | `0x001f` | shim 被进入（LK 确实跳到了 `0x80080000`） |
| 黄 | `0xffe0` | 源区 clean 到 PoC 完成 |
| 绿 | `0x07e0` | 4 MiB 固件卷已拷到 `FD_BASE` |
| 品红 | `0xf81f` | 拷贝已 publish（`ic iallu`） |
| 白 | `0xffff` | 看门狗已布好，正要进 SEC |

第一版只刷顶部 16 像素高的一条，最后一种颜色还是白色 —— 而 LK 的启动 logo 背景就是白的，
所以那版等于什么都没画。**整屏是必须的。**

### 9.3 `SHIM_SELFTEST`：把「跳没跳」和「会不会复位」分开测

`SELFTEST=1 sh uefi/build-e5.sh DEBUG`（或直接 `SHIM_FLAGS=-DSHIM_SELFTEST`）会编出
一个停住不走的 shim：刷完白色之后红/绿整屏交替，每 0.4 秒一次。这样两件事同时可见 ——
CPU 活着（颜色在变）、看门狗到期（颜色不再变）。

闪烁次数是**有界的**（`#define SHIM_BLINKS 300`，约 4 分钟），闪完照样掉进 SEC，
免得一个没人看着的实验永远停在那里。`SHIM_FLAGS` 是新增的通用口子，例如
`SHIM_FLAGS="-DNO_WATCHDOG -DSHIM_SELFTEST"` 可以完全绕开看门狗。

### 9.4 结论（2026-10-23 07:21 实测）

**整屏红绿交替闪 —— 人眼确认。** 这一条同时钉死了三件事：

1. LK 的 `start_linux` 确实跳到了 `0x80080000`，我们的 shim 在跑；
2. `0xbe000000` 这块缓冲在 LK 交接之后**仍然被 DPU 扫描输出**（M0 阶段的结论对 shim 同样成立）；
3. 背光、面板时序在交接之后都还活着。

在此之前一直以为「看不到 = 没跑」，其实是**屏幕全黑 = 板子在反复重启**（见 9.5），
不是没有输出。

### 9.5 黑屏的真相：0.7 秒一次的重启风暴

那一版（刷完整屏色块后 `br` 进 SEC）用户看到的是「屏幕黑的，一直在间断的震动」。`uboot_log`
里对应的一段是：

```
06:45:04  Booting slot_b
          [start_linux] start linux at PC:0x80080000, dt_addr:0x86000000
06:45:13  LK time is ...          <- 9 秒后已经在重新启动 LK
06:45:14 / 06:45:15 / 06:45:16 / 06:45:25 / 06:45:40 ...
06:54     ANDROID: Booting slot_a  <- 靠回滚脱身
```

复位原因寄存器 `hw_rst_mode==2` / `hw watchdog int raw status 0x8`（只有 bit3 `RST_RAW`，
没有 bit0 `INT_RAW`）说明是**看门狗直接复位**，而且从交接到复位不到 1 秒 —— 屏幕上的色块
还没画完就被清掉了，所以人眼什么都看不到。震动是马达在每次启动时被短暂驱动。

对照实验：**只 park 不进 SEC** 的那一版（`SHIM_SELFTEST`）活了 4 分 51 秒才被看门狗收走。
两者唯一的差别是最后那一条 `br x5`。所以故障点在 **SEC 内部**，不在跳转本身。

### 9.6 工具链注意事项

这台机器的 root 是 Magisk，`ro.debuggable` 现在是 0，所以 `adb root` 会直接拒绝：

```
adbd cannot run as root in production builds
```

特权操作要写成 `adb shell "/debug_ramdisk/su -c '<命令>'"`，读分区用
`adb exec-out "/debug_ramdisk/su -c 'dd if=/dev/block/by-name/... bs=...'"`。

## 十、当前状态


- [x] EDK2 + Mu + Silicium 构建链在 macOS 上跑通（CLANGDWARF）
- [x] E5 平台包与 SoC 包成型，`sh uefi/build-e5.sh DEBUG` rc=0，产出 4 MiB FD
- [x] BootShim 搬运 + SEC 入口 + ramoops 输出在实机确认（第一次实机就打印到 `Failed to Configure MMU`）
- [x] MemoryMapLib 39 个 region 全部 4 KiB 对齐（正则复核过 overlaps: 0）
- [x] **LK 确实跳到 `0x80080000`**，帧缓冲在交接后仍被扫描输出（07:21 红绿交替闪，人眼确认）
- [x] 看门狗确实能复位（`hw_rst_mode==2` + `hw watchdog int raw status 0x8`）；但生效的是 LK 留的
      约 300 秒超时，shim 自己的 60 秒因缺 `LD_BUSY` 轮询而没写进 LOAD
- [x] 看门狗布置与 EndOfDxe 主动复位已编进镜像
- [ ] **未定位**：进 SEC 后不到 1 秒就被看门狗复位（park 版能活 4 分 51 秒，只差 `br x5` 这一条）
- [ ] DXE 派发是否完成（至今没拿到任何 `Silicium`/`DXE` 日志）
- [ ] BDS / UFPLoader / DarwinBoot 尚未启用
- [ ] eMMC 没有 EDK2 host 驱动，`SdDxe`/`EmmcDxe`/`Fat` 先不挂进 FV（M5 处理）
- [ ] 只有单核跑 DXE（FD 里没有 `ArmPsciMpServicesDxe`），XNU 的 SMP 由 UniXNU 的 ArmCpu 负责

### 10.1 实机运行记录（2026-10-23）

| 时间 | shim 开关 | 现象 | 复位原因 | 结论 |
| --- | --- | --- | --- | --- |
| 01:46 | 无（进 SEC） | 屏幕无任何输出 | 无记录，之后不再写日志 | 最早一次「看起来挂死」 |
| 06:34 | `SHIM_SELFTEST`（无界闪烁，已废弃） | 屏幕无色带 | `hw watchdog int raw status 0x9`，4 分 51 秒 | 只 park 不进 SEC，能活满 LK 的约 300 秒 |
| 06:45 | 无（进 SEC） | 整屏黑 + 间歇震动 | `hw watchdog int raw status 0x8`，0.7 秒一次 | 故障点在 SEC 内部 |
| 07:21 | `-DNO_WATCHDOG -DSHIM_SELFTEST`（`SHIM_BLINKS 300`） | 整屏红绿交替闪，人眼确认 | 闪完后进 SEC 同样被复位 | LK 跳转、DPU 扫描、背光时序全部成立 |

07:21 那一轮之后设备回滚到 slot A，收尾状态：

- `misc` offset 0x800 回到原厂记录 `5f61 0000 4243 4142 0102 0000 9f00 1e00 ... 0be1 7146`
  （slot 0 prio 15 / tries 1 / succ 1，slot 1 prio 14 / tries 1 / succ 0），即 LK 已把 slot B 判失败。
- `boot_b` 仍是我们的镜像：头 `ANDROID!`、`kernel_size = 0x400240`（4194880）、偏移 0x1000 处
  `10 00 00 14`（shim 的 `b`）、偏移 0x1008 处 `00 00 08 00`（text_offset 0x80000）。
- 物理断电重启后 RTC 归零，`uboot_log` 里时间戳变成 `1970-01-01_08-0x-xx`，此前那一段被环形缓冲
  覆盖，所以 07:21 那一轮自己的复位现场没能留下 —— 长重启风暴会吃掉日志，取证要及时。
