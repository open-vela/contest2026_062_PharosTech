# 启动与打包

## 交接合同

画出实际启动链，并为每个阶段记录存储位置、加载/入口地址、镜像格式、
验证行为、EL、栈及已占用内存区域。loader、BL31、OP-TEE 和 N-Boot
分别有自己的地址合同，不能只记 NuttX 的一个 load address。

同系列芯片的 DDR blob、SPL、固件与内存布局不能直接互换。
“二进制可下载”不证明版本兼容或允许重分发，应逐件核实。

## FIT：先定位是谁失败

SPL 报 hash 错或仅大镜像失败时：
1. 确认失败位于 SPL、BL31、N-Boot 还是 NuttX，并核对实际启动版本。
2. 检查 FIT header 的 totalsize、段 data-position/data-offset、load/entry。
3. 对照 SRAM/RAM 和 loader 工作区，检查外部载荷的对齐与扇区读取规则。
4. 比较每段在原文件和最终组盘中的 hash，确认打包后没有被二次修改。

大内嵌 FIT 可能占用 loader 工作区；external-data FIT 也可能因为段位置、
对齐或后处理改变 header 而失败。不要把“-E”当万能修复。
K7 的可重复经验和反例见案例 E1/E2。

## 无串口的常见伪象

读取 ELF program headers 和原始镜像开头，检查 build-id/note 是否占据入口。
确认本次启动的是新介质、新槽位和新镜像，避免把旧 eMMC 固件当成 SD
构建失败。先用轮询串口缩小范围，再判断完整驱动和 console 配置。

## 现行与历史更新方式

K7 当前产品为 N-Boot A/B；build_sd.sh 生成整盘包，普通 NuttX 更新经
nbootctl。旧 k7flash 固定 uboot 槽写入路径只适用于对应历史直接 BL33
布局，不能直接用来更新 N-Boot 系统。

写入前检查：目标介质、槽位、镜像类型、大小边界、当前版本与恢复路径。
写后从介质读回核对；失败保留运行态，不自动重启进半写镜像。
A/B 的 trial/confirm 具体语义取决于当前启动器和 nbootctl 版本，
不能套用另一实现的命令或激活策略。

成功热更新不证明掉电原子性；只有执行过中断故障测试才能声称该项。
