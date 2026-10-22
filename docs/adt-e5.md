# E5 的 Apple 设备树（ADT）

XNU 的 DarwinBoot 不读标准 FDT，它读 Apple 格式的设备树（ADT）。UEFI 侧的
`EFI_DTB_TABLE` 里那棵标准 FDT 与它是两回事：前者给 UEFI 驱动用，后者给内核用。
本文记录 ADT 的编码、XNU 侧对它的硬性要求，以及 E5 对应的取值。

生成器：`uefi/tools/make-e5-adt.py`（`--dump` 可把产物反解成缩进列表自检）。

## 一、编码

```
node := u32 property_count | u32 child_count
        property_count * { char name[32]; u32 length; u8 value[length]; pad to 4 }
        child_count * node
```

- 所有数值小端。64 位地址写成两个 u32 cell，**低位在前**（与标准 FDT 的大端相反）。
- 每个节点自带一个 `name` 属性（含结尾 NUL），它排在属性表第一位。
- 没有字符串表，属性名内联在 32 字节的定长字段里。

DarwinBoot 调用 `AdtNode(Tree, TreeSize, &Offset, 0, 3, ...)` 解析后要求
`Offset == TreeSize`，即**整棵树必须被完整消费**，多一个字节都算失败。

## 二、XNU 侧的要求（`ArmDTPlatform.cpp` + `docs/porting/device-tree.md`）

`ArmDTPlatform` 是内核里的 `IODTPlatformExpert` 子类，靠根节点名 `device-tree` 匹配。
`configure()` 里逐条硬性检查：

| 检查 | 缺了会怎样 |
| --- | --- |
| `/cpus` `/arm-io` `/defaults` `/options` 四个节点 | 打印 `required /cpus, /arm-io, /defaults or /options node is absent` 后返回 false |
| `/chosen` 的 `nvram-bank-size`（u32）与 `nvram-proxy-data`（非空） | 打印 `/chosen does not supply a usable CHRP NVRAM proxy` 后返回 false |
| `/chosen/memory-map` 的 `RAMDisk` `PackedRAMDisk` `TrustCache` 三个 16 字节占位符 | DarwinBoot 返回 `EFI_COMPROMISED_DATA` |
| `/chosen` 的 `unique-chip-id` 与 `chip-id` | `gethostuuid()` 会无限等待 |
| 多于一个 CPU 时的 `/psci` 节点 | 副核起不来 |

`createDeviceNubs()` 把 `arm-io` 的**每一个**子节点都发布成 `IOPlatformDevice`，
所以 GIC、UFS 主控、PHY 这些必须在 `arm-io` 下可见，否则驱动拿不到资源。

`ArmDTPlatform::deleteList()` / `excludeList()` 返回 `"()"` 而不是 NULL ——
XNU 的 `processTopLevel()` 把 NULL 解释成「删掉所有子节点」，返回空表才是「一个都不删」。

## 三、E5 的取值

全部来自实测或实机 FDT，没有猜的：

| 属性 | 值 | 依据 |
| --- | --- | --- |
| `timebase-frequency` | 26000000 | M0 报告 `CNTFRQ_EL0` |
| `dram-base` / `dram-size` | 0x80000000 / 0x7ffff000 | LK 日志 `/memory` 节点 |
| GICD / GICR | 0x12000000 / 0x12040000 | 原厂 FDT、M0 实测 |
| UART1 | 0x20210000 | 原厂 FDT、M0 实测 |
| CPU 数 | 8（6×A55 + 2×A76） | 原厂 FDT `/cpus` |
| `psci/method` | `smc` | M0 实测 PSCI 0.2 走 SMC |
| `crypto-hash-method` | `sha2-384` | 与 UniXNU-sheng 一致 |
| `darwinboot-memory-size` | 0x20000000（512 MiB） | 必须在 256 MiB–8 GiB 之间、2 MiB 步进 |

`arm-io` 只开**一个**总线窗口 `ranges = <0, 0x12000000, 0x20000000>`，覆盖
0x12000000–0x32000000。子节点的 `reg` 是窗口内偏移：

- `intc`：GICD 偏移 0x0 长度 0x20000、GICR 偏移 0x40000 长度 0x100000
- `uart`：偏移 0x0E210000（= 0x20210000 - 0x12000000）长度 0x1000
- `emmc`：偏移 0x10200000（= 0x22200000 - 0x12000000）长度 0x10000

窗口外的设备（例如 CP、WCN）只能由驱动用板级常量取物理地址。

## 四、两个未定项

### 4.1 CPU 的 `reg` 到底是 Affinity0 还是 Affinity1

原厂 FDT 把 8 个核写成 `cpu@0` `cpu@100` … `cpu@700`，`reg = 0x0, 0x100, … 0x700`
（即 Affinity1 = 序号、Affinity0 = 0）。但 M0 在实机上读 8 个 GICR 帧的
`GICR_TYPER`，高 32 位的 Affinity_Value 读回来是 `0,1,2,…,7`（即 Affinity0 = 序号）。

两者不可能同时成立，而 PSCI `CPU_ON` 的参数必须是真实亲和值。这个矛盾**必须先读
副核的 `MPIDR_EL1` 才能定论**（M0 的 SMP 探针把机器搞崩过，暂缓）。
生成器因此留了开关：`--cpu-affinity fdt`（默认，`reg = 序号 << 8`）或
`--cpu-affinity index`（`reg = 序号`）。定论之后删掉另一个分支。

### 4.2 根卷怎么匹配

`/chosen/root-matching` 是一段 XML，告诉 XNU 用哪个 `IOMedia` 当根。E5 现在没有
UEFI 下的 eMMC 驱动，所以短期内只能走 RAM 根（`osenvironment = ramdisk`，
根卷由 DarwinBoot 从 payload 里的 Data 镜像提供），等 eMMC 块驱动落地后再改成
UUID 匹配（`--root-uuid`）。

## 五、用法

```
# 生成（默认 8 核、512 MiB DarwinBoot 窗口、main 环境）
python3 uefi/tools/make-e5-adt.py --output uefi/build/e5.adt

# 反解自检：把产物读回来打印整棵树，并检查是否有尾随字节
python3 uefi/tools/make-e5-adt.py --output uefi/build/e5.adt --dump

# RAM 根 + 单核（调试早期内核入口时用）
python3 uefi/tools/make-e5-adt.py --cpus 1 --osenvironment ramdisk
```

产物大小约 12.7 KiB，远低于 DarwinBoot 的 16 MiB 上限。每次生成 `random-seed`
都不同，这是刻意的：XNU 要求每次启动的种子唯一。
