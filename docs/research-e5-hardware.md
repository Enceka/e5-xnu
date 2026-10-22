# Rongyue E5（Unisoc UMS9621）硬件与引导链事实报告

> 来源：只读子代理对 `/home/hema/Workspace/e5-linux` 与 `/home/hema/Workspace/e5` 的梳理（2026-10-22），协调者已对其中 5 项支撑性事实（GICv3/PSCI/定时器/UART 地址/内存节点/LK 加载地址）独立抽查复核，全部吻合。
> 标记：**【C】已确认（附路径） / 【I】推断 / 【U】未验证**。
> 缩写：E5L = `/home/hema/Workspace/e5-linux`；VK = `E5L/kernel_sprd_ums9158`（vendor 5.15.211 内核树）；PROBE = `/home/hema/Workspace/e5/kernel_probe`（实机 FDT dump）。

## 1. SoC
- 【C】UMS9621 = Unisoc **Qogirn6lite**：VK `arch/arm64/boot/dts/sprd/ums9621-base.dts` 头注释；实机 chipid "UMS9621-AA"（PROBE/e5.dts hwfeature）。
- 【C】8 核 big.LITTLE：CPU0–5 = Cortex-A55、CPU6–7 = Cortex-A76，全部 `enable-method="psci"`（VK `.../sprd/ums9621/ums9621.dtsi:53–128`）。
- 【C】GPU = "sprd,mali-natt"（VK `.../sprd/ums9621/gpu-natt.dtsi`）；Linux 侧无 Mesa 驱动，显示走 llvmpipe + DSI framebuffer/DRM planes（E5L docs/FINDINGS.md）。Mali IP 代次【U】。
- 【C】频点无 DT OPP 表，档位/电压经原厂 SIP 固件接口读取（E5L docs/CPU-CONTROL.zh-CN.md）；具体频点【U】。
- 【C】UMS9158 是 vendor 内核源码包的命名（Enceka/kernel_sprd_ums9158，Android13 GKI 5.15.211），E5 板级名 **ums9158_1h10**；UMS9158 芯片本身定义【U】。

## 2. 中断控制器 / 定时器 / 串口（XNU pexpert 对接面）
- 【C】**GICv3**：GICD@0x12000000（size 0x20000）、GICR@0x12040000（size 0x100000），redistributor-stride 0x20000，PPI9 maintained IRQ（ums9621.dtsi:177–190）。实机 FDT 未见 gic-its 节点 → MSI/PCIe【U】。
- 【C】arch timer："arm,armv8-timer"，PPI 13/14/11/10（ums9621.dtsi:192–199）。
- 【C】PMU："arm,armv8-pmuv3"，SPI 177–184 ×8 核（ums9621.dtsi:201–210）。
- 【C】UART×4："sprd,ums9621-uart","sprd,sc9836-uart"，ap-apb 总线基址 0x20200000 → uart0=0x20200000、**uart1=0x20210000**（PROBE/e5.dts:873–909）。
- 【C】控制台：stock stdout-path=serial1:921600n8；主线镜像固定 `earlycon=sprd_serial,0x20210000,115200n8 console=ttyS1,115200n8` + `CONFIG_CMDLINE_FORCE=y`（upstream/e5-mainline.config:70-71）。

## 3. 内存与存储
- 【C】RAM ≈ **2 GiB**：实机 FDT `memory@80000000 reg=<0 0x80000000 0 0x7ffff000>`（PROBE/_dev.dts:7829）。
- 【C】存储 = **eMMC**（bootargs `sprdboot.flash=emmc`）：sdio@22200000 "sprd,sdhci-r11"，8-bit HS400 enhanced-strobe + CQHCI + ICE（inline crypto）（PROBE/e5.dts:1313–1345）。UFS 节点缺席【C·缺席证据】→ UniXNU 的 NativeUfs 不能直接复用。
- 【C】SD 卡槽 sdio@22230000（SDR104/SDR50）；Wi-Fi 专用 SDIO sdio@22220000。
- 【C】SD 多启动：boot_b 分区（64MiB）一个共享 kernel/initramfs 选系统；注册表在 e5boot 分区（ext4）：`systems/<sys>/<slot>` PARTUUID + default/next + kernel-release 校验（openwrt/MULTIBOOT.md、boot/init）。
- 【C】boot-linux-slotb.json：boot 镜像 sha256/kernel+ramdisk 尺寸/cmdline/misc 两槽 32B hex（A=5f61…、B trial=5f62…）、persist_log 偏移 56MiB（boot/build-boot-image.py）。

## 4. 引导链（核心结论：无 UEFI，入口在 LK）
- 【C】原厂链：bootrom → FDL（fdl1/fdl2，Workspace/e5/fdl1-dl.bin、fdl2-dl.bin）→ "uboot" 分区 = **LK 类 aboot** → Trusty TEE（sml/teecfg/trustos）→ vbmeta/AVB → boot/init_boot/vendor_boot（A/B 槽，misc@0x800 BCAB bootloader_control）。本机已解锁，AVB 不强制（Magisk 改摘要仍可启动）。
- 【C】Linux 接管方式：**逐字节保留 stock boot 头/vbmeta/AVB footer**，只替换 boot_b 的 kernel（Image.lk = 主线 Image + wrap-image.py 跳转 stub）+ initramfs + cmdline。**LK 把 kernel 固定装载到 0x80080000 并跳转**；stub 使真内核跑在 0x80200000；DTB 由 LK 经 x0 传入（原厂 vendor FDT），主线 Linux 不自带 DTB。
- 【C】boot.img 格式 ANDROID! v4（header_size 1584）；stock boot_b kernel_size=46,754,304B、ramdisk_size=0；`Workspace/boot_current_20260905.img` 为 2026-09-05 原厂 boot 槽整 64MiB dump。
- 【C】uficode/ = Android 侧 AT 诊断工具（Go/Kotlin）；UFI-TOOLS/ = Web 控制面板（:2333）。均为用户态，与引导无关。

## 5. 设备树
- 【C】vendor 树主 DTS：ums9621-base.dts + ums9621/ums9621.dtsi + ump9620/21/22 PMIC；E5 板级 = `e5-rongyue-overlay.dts`（/plugin/ overlay，自述由实机 FDT（kernel_probe/e5.dts）重建，板级模板 motorola-sprd-uoa34）。
- 【C】根 compatible："sprd,ums512-base","sprd,ums9621"；soc 节点 "simple-bus"。
- 【C】实机 chosen：lcd_name=lcd_st7365p_mipi_hdp、lcd_size=480x320、`sprdboot.flash=emmc`、slot_suffix=_a、modem=shutdown。
- 【C】**PSCI 0.2**（"arm,psci-0.2"，无函数 ID → 标准 SMC 调用约定，ums9621.dtsi:136–137）。
- 【C】reserved-memory：cp-mem@88000000 436MB（5G CP）+ nrphy-iqmem；1h10 overlay 另加 protected/oemcrypto/widevine CMA 360MB/faceid。
- 【C】主线树 linux-lts-e5 无 ums9621 DTS → 使用 LK 传入的原厂 FDT【I·强】。

## 6. 内核配置（upstream/e5-mainline.config fragment；完整 work/build/7.2.11/.config）
- 【C】SERIAL_SPRD(+CONSOLE/EARLYCON)=y；ARM_GIC_V3=y、ARM_GIC_V3_ITS=y；ARM_PSCI_FW=y；CMDLINE_FORCE=y；SMP、NR_CPUS=8。
- 【C】SPRD_UMS9621_CLK、MMC_SDHCI_SPRD、SPRD_SYSTIMER、SPRD_QOGIRN6LITE_USB2_PHY、SPRD_BC1P2 均 y。
- 【C】UNISOC 外设（WCN/SIPC/modem/disp）全为 m（模块后加载）。

## 7. 关键外设（型号+证据）
- 显示：DPU@0x31000000 "sprd,qogirn6lite-dpu"、DSI@0x31300000、GSP@0x31100000、IOMMU@0x31001800；面板 ST7365P 480x320（Sitronix，MIPI）。
- 触摸：SPI1（gpio13 irq/gpio14 reset），compatible 混 focaltech,fts / novatek,NVT-ts-spi / synaptics,tcm-spi；实机节点带 novatek*，分辨率 720x1612（0x2d0×0x64c）。实际触控 IC【U】。
- 充电：Awinic AW32257（i2c2-006a，aw322xx_chg，CONFIG=m）；燃料计走 PMIC FGU。
- PMIC：UMP9620+UMP9621+UMP9622（ADI 总线 "sprd,ums9620-adi" reg 0x400000）。
- 基带：片内 5G CP（cp-mem 436MB@0x88000000），AP↔CP 走 SIPC（mailbox/SIPA）。OpenWrt 用自研 Rust 守护 unisoc-cpd；Debian 侧 vendor rild 目录存在但 stat ENOENT（环境异常），内容【U】。
- WCN：Marlin3Lite UMW2652 combo（SDIO1）：WiFi sc2355、BT ttyBT、GNSS gnssmodem.bin、FM。
- USB：musb/udc@0x64000000；gadget = NCM 网 + CDC-ACM ttyGS0 控制台。
- 其他：指纹 microarray afs121、闪光灯 aw3641、AON GPIO 按键、PWM 背光。

## 8. 现有构建/刷写流程（e5-linux）
- 【C】内核：upstream/build.sh（Docker；E5_KERNEL_TREE/E5_RELEASE/E5_LANDSCAPE/E5_KERNEL_MANIFEST）→ Image、Image.lk、modules。
- 【C】boot 镜像：boot/build-boot-image.py（--stock-boot dumps/boot_b.img --misc-head dumps/misc-head.bin …）；boot/init 52KB initramfs。
- 【C】OpenWrt flash 包：openwrt/make-flash-bundle.sh；桌面安装器 installer/（Tauri+Python，ADB 刷 TF/boot_b 前 56MiB/misc 槽位）；tools/android-bootchain.sh 同步两槽 OTA 防错槽。

## 9. 目录关系
- 【C】`Workspace/e5` = 原始研究区（实机 dump、TWRP 树、vendor 模块、FDL、固件包）；`e5-linux` = 产品化仓库（sprd-linux-devs/e5-linux）。

## 10. 调试通道
- 【C】硬件 UART：ttyS1=UART1@0x20210000（stock 921600 / 主线 115200）；物理焊点位置【U】（docs 无记录）。
- 【C】免拆机：USB CDC-ACM ttyGS0 @115200（serial-getty root/root，tools/e5-serial.sh）；ramoops@0xfff80000（256KB）；boot_b 顶 8MiB 持久 init 日志；uboot_log 分区。

## 附：对 XNU 移植直接相关的事实速记（仅事实）
1. LK 入口约束：kernel 装载 **0x80080000**、arm64 Image 头（"ARMd"）、DTB 经 x0 传入；wrap-image.py 的 branch-stub 证明**任意自定位入口代码**可从此入口启动。
2. 无 UEFI：bootrom→FDL→LK→kernel，与 UniXNU 的 UEFI→XNU 路径不同层级。
3. pexpert 对接面：GICv3（GICD 0x12000000/GICR 0x12040000，无 ITS）、armv8 timer（PPI13/14/11/10）、sprd UART@0x20210000、PSCI 0.2 SMC、内存 2GiB@0x80000000（顶 0xfff80000 ramoops 保留）。
4. 存储 eMMC（非 UFS）；GPU Mali "natt"（非 Adreno）——与 UniXNU 现有 Native* 驱动族不重叠。

## 未验证项清单
CPU 实际频点；Mali IP 代次；触控 IC 实物型号；UART 物理焊点；vendor rild 目录内容；GIC ITS/PCIe 实际可用性。
