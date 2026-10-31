---
name: nboot-control
license: Apache-2.0
description: 操作与维护 N-Boot（KICKPI-K7 / RK3576 的 U-Boot 下游发行版）：A/B 槽与 bootctrl 元数据、NuttX/AMP 双域镜像的制作与刷写、fastboot 与串口恢复通道、eMMC/SD 介质选择，在 openvela/NuttX 侧用 nbootctl 读取启动交接、控制重启目标、从运行中的系统写槽（OTA），以及用 bootamp 内存启动迭代 AMP 镜像。用于刷机、槽位切换、OTA、启动排错、系统卡死自救、N-Boot 自身的升级；不用于应用层开发或未确认目标介质的盲写。
---

# N-Boot 操作与控制

N-Boot 是 Pharos Tech 为 Nyabula/KICKPI-K7 维护的 U-Boot 下游发行版，
不是从零重写的引导器。它保留了 U-Boot 的历史与许可证，新增的是板级
适配与启动管理：双域 A/B、校验写入、USB 恢复、以及给操作系统的启动交接。

**先确认介质，再动手写。** 板子上同时可能插着 SD 和 eMMC，两者用同一套
GPT 布局，选错介质不会报错，只会静默写坏另一块盘。

## 开始时只收集影响下一步的信息

确认：目标介质（SD `mmc0` / eMMC `mmc1`）、当前板子在哪一层（N-Boot 控制台
/ fastboot / nsh）、要写的是哪一类载荷（主域固件 / AMP FIT / N-Boot 自身 /
分区表 / loader）、串口号、以及能否冷启动（有些操作只有冷启动才生效）。

不清楚介质时先读回 LBA 0 与 LBA 16384 判断盘上有谁，不要猜。

## 三个必须记住的机制

**1. N-Boot 自己选不了自己。** N-Boot 由 BootROM 的 SPL 加载。BootROM 从哪块
介质加载 SPL 是上电时定的。SD 卡在槽里且布局有效 → 永远从 SD 起，
即使 eMMC 上有更新的 N-Boot。要让新写的 N-Boot 生效，必须
**冷启动 + 移除另一块介质**。

**2. 介质选择是"SD 优先，回退 eMMC"。** `nboot_storage_boot_devnum()`
依次探测 SD(0)、eMMC(1)，第一个布局合法的胜出。合法 = `uboot`/`trust`/
`bootctrl`/`nuttx_a`/`nuttx_b` 的**名称、起始扇区、扇区数三者精确匹配**。
不匹配的盘视为"无关"，不会被选中。

**3. 空白盘上是死锁。** `fastboot oem board:flash:*` 与 `flash nuttx_a|b`
都要求上述布局已存在。空白盘没有分区表 → 一律
`FAIL invalid recovery partition layout`。而 `MiniLoader` 在 LBA 64，
不属于任何分区，`fastboot flash <part>` 按名字找分区，够不到它。
空白盘起步必须走 [emmc-provisioning.md](references/emmc-provisioning.md)。

## 选择资料

| 当前任务 | 按需阅读 |
|---|---|
| 命令与 OEM 子命令速查 | [commands.md](references/commands.md) |
| bootctrl 元数据格式、槽位语义 | [bootctrl.md](references/bootctrl.md) |
| 从零给空白盘装系统 | [emmc-provisioning.md](references/emmc-provisioning.md) |
| 制作/刷写 NuttX 与 AMP 镜像 | [images.md](references/images.md) |
| 进不去 N-Boot、板子无响应、AMP 静默 | [recovery.md](references/recovery.md) |
| NuttX 侧读启动信息、控制重启、动词在哪棵树 | [nbootctl.md](references/nbootctl.md) |
| 系统在跑时升级固件（OTA）：要设计/调试上传-校验-写槽-确认流程，或经 WiFi 传固件失败 | [ota.md](references/ota.md) |
| 迭代 AMP 镜像而不写盘；`bootamp` 拒绝 FIT 或启动后复位；把更大的 openvela 固件放进 AMP | [bootamp-ram.md](references/bootamp-ram.md) |
| 已知缺陷与规避（含 CI 契约串陷阱、热重启后 eMMC 起不来、卡死自救、`adb shell` 丢输出） | [known-issues.md](references/known-issues.md) |

## 工作路径

判断当前状态 → 选对介质 → 用校验路径写 → 回读验证 → 冷启动确认。

这是依赖顺序，不是每次都全做。已有可信实测结论的环节不必重跑。

**始终优先选择带校验的路径**：

- 写主域固件用 `fastboot flash nuttx_a|b <image>`（走到
  invalidate-before-write → 介质回读 SHA-256 → 更新 bootctrl 的完整路径）。
- 写 AMP FIT 用 `fastboot flash amp_a|b <itb>`，或
  `fastboot oem board:flash:amp_a|b`——两条最终到同一个函数。
  **旧版 N-Boot 只有 OEM 那条**：`fastboot flash amp_a` 会裸写分区，
  不写 size/version 元数据。见 [known-issues.md](references/known-issues.md) #2。
- 写 N-Boot 自身用 `fastboot flash nboot <4 MiB FIT>`（校验 6 个子镜像后
  原位更新并回读）。
- 通用 `fastboot flash <name>` 对任何 GPT 分区都可用，但**没有**上述校验。
- 系统在跑、不进 N-Boot 时用板上的 `nbootctl stage`：先让整个文件落盘并两端对过
  SHA-256，再写非运行槽、回读、记元数据。**它写完即激活**，没有单独的 activate。
  见 [ota.md](references/ota.md)。
- AMP FIT 先 `fastboot stage` + `bootamp 60000000 <size> check`，再去掉 `check`
  真跑，全程不写盘。串口命令**只用单个 CR 结尾**。
  见 [bootamp-ram.md](references/bootamp-ram.md)。

**N-Boot 不是看门狗，也没有启动计数回滚。** 选槽只看 `priority`
（`tries_remaining` 是死字段）；只有**镜像校验失败**才会同轮落到另一槽。
能过校验但起来后挂死的固件会被一直选中——出路是串口强制 panic 自救 + 人工换槽，
见 [known-issues.md](references/known-issues.md) #13。

⚠️ host 侧 `flash` **必须带文件名**：`fastboot flash amp_a`（漏了文件名）
会报 `cannot determine image filename`，看着像分区不存在，其实是语法错。

写完立刻回读核对。

## 工具

```sh
# 写入任意 LBA 并做全量 CRC32 校验（空白盘起步的唯一路径）
python3 scripts/nboot_flash.py --dev 1 write 0x4000 uboot.img
python3 scripts/nboot_flash.py --dev 1 gpt layout.txt
python3 scripts/nboot_flash.py --dev 1 verify 0x4000 uboot.img
python3 scripts/nboot_flash.py --dev 1 read 0 4          # dump 扇区

# bootctrl 结构审计（只读，不碰盘）
python3 scripts/check_bootctrl.py bootctrl.img
python3 scripts/check_bootctrl.py --layout disk.img      # 校验 GPT 是否符合 N-Boot 要求
python3 scripts/check_bootctrl.py --selftest             # 自检
```

`nboot_flash.py` 的每步都做三段校验：stage 到 RAM → 板子算 RAM 的 CRC32
比对 → `mmc write` → **从介质回读再算 CRC32** 比对。
它同时解决三个问题：空白盘无分区表、MiniLoader 在分区外、串口传不了大文件。
原理见 [emmc-provisioning.md](references/emmc-provisioning.md)。

## 边界

- 直接 `boot`、`set_active` 被拒绝（提示改用 `board:activate:<slot>`）。
- `oem run`、UUU、任意 OEM 执行未开放。
- 当前树**没有**写授权/口令/超时门禁；不要假设有。
  `getvar nboot-medium` 是唯一新增变量，`current-slot` 是硬编码 `"a"`，
  不代表真实槽位——真实槽位从 PMU1 GRF 交接记录读。
- N-Boot 的自更新不宣称断电原子性（vendor SPL 的 2 MiB 候选间距与 4 MiB
  FIT 布局重叠，只能单区域原位更新）。

## 报告

向用户报告：当前板子在哪个介质与哪一层、写了什么、校验结果、还剩什么没验证。
没有实测过的结论标"仅推断"。写盘失败时保留原始报错，不要改写。

用户授权构建与测试不自动授权 push、发布或合入 PR。

## 两条容易犯的验证错误

**① 不要用"跑通了"当"修好了"的证据。**

要证明一个修复有效，必须做**修复前后的对照**。测一个本来就正常的路径、
看到预期结果，那什么也没证明。

实例：改 fastboot 退出顺序后，我测了"写入 → 启动 NuttX → 查设备"三轮，
都正常，就宣称修复有效。但那三轮测的是 NuttX 启动后的状态，而 NuttX
本来就会顶掉那个残留设备节点——**有没有修复都一样**。真正该测的是
"停在 N-Boot 不启动 NuttX"的状态。见 [known-issues.md](references/known-issues.md) #1。

**② 现象要归到正确的实体上。**

主机的 USB 设备列表里可能同时有**与板子无关**的条目。用 `InstanceId` 的
父设备段与序列号判断归属，不要看到"未知 USB 设备"就认定是板子的。

同样地，"板子没反应"要先确认**串口被谁占着**——两个进程抢同一个串口时，
发出的命令可能根本没人收。

## 改了 `drivers/fastboot/fb_command.c` 之后

CI 的 `Verify binary contract` 步骤会 grep 链接后的二进制：

```sh
strings build/u-boot-nodtb.bin | grep -q 'flash:nuttx_a'
strings build/u-boot-nodtb.bin | grep -q 'partition is not in recovery allowlist'
strings build/u-boot-nodtb.bin | grep -qx 'stage'
strings build/u-boot-nodtb.bin | grep -q 'N-Boot image verified after write'
```

**这些字符串是契约**：它们证明校验路径确实被编进了固件。
把编译期字面量改成运行时拼字符串（`snprintf`）会让这个检查失败——
本地 `make` 不会告诉你，CI 会。

改这个文件后本地先跑一遍：

```sh
strings u-boot-nodtb.bin | grep -qx 'flash:nuttx_a'
```
