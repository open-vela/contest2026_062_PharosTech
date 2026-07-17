# KICKPI-K7 板上固件热更新 架构 (K7 OTA Pro)

> 目标:板上运行的 NuttX 接收新固件 → 写 SD 卡 uboot 槽 → 重启生效。**不拔卡**。
> Pro 版(2026-07-14): 传输主通道换 **ADB(USB 高速)**,串口 Ymodem 降为备用。
> 健壮版(2026-07-17): ADB 通道加**传输后板上大小回读校验 + 重试**(截断绝不落盘);
> **去掉自动回退 Ymodem**(那条路会 `usbsw off` 切走 adb,一旦 Ymodem 再失败板子会
> 卡在 rb 接收态、串口失同步、只能断电)。Ymodem 仅在显式 `--ymodem` 时才走。

## 0. 一图
```
PC 端                              板上 (NuttX)
--------                           -----------------
build_sd 出 uboot_nuttx.img(FIT)
  │ 默认: adb push (USB)
  │   └ push 后板上 ls 回读字节数, 与本地比对; 不一致=截断 → 删残file 重试
  │ 显式 --ymodem: 串口 (rb / 1.5Mbaud, 边际不稳, 慎用)
  └───────────────────────────────► /tmp/fw.img (tmpfs)
                                       │ 默认: adb shell k7flash  --ymodem: 串口触发
                                     k7flash /tmp/fw.img   ← 自研核心
                                       ├ 校验: FIT magic + 大小
                                       ├ 护栏: 目标写死 /dev/mmcsd0 @sector 16384
                                       ├ 写入 (SDMMC host 写路径)
                                       ├ 读回逐扇区校验
                                       └ boardctl(BOARDIOC_RESET) → PSCI 重启
                                                                     └► 新固件启动
  刷完若给 --port,自动经串口拉起 adbd → 下一轮 OTA 继续走 ADB(全闭环)。
```

## 0.1 用法
```
python k7_ota.py --img uboot_nuttx.img                 # ADB(默认, 推荐)
python k7_ota.py --img uboot_nuttx.img --port COM4     # 刷完再经串口拉 adbd
python k7_ota.py --img uboot_nuttx.img --retries 5     # 弱链路多给几次重试
python k7_ota.py --img uboot_nuttx.img --ymodem --port COM4   # 显式串口 Ymodem
```
**默认 ADB 通道绝不自动回退 Ymodem。** 无 adb 设备时直接报错退出(提示检查
USB 线/口、板上 `adbd &`),不去动 `usbsw`——板子始终保持 adb 可达, 可直接重试。

坑与对策(实测):
- **push 截断**: 换线/换口后 USB 可能降速到 KB 级, 大文件 push 中途可能被截。
  工具在 k7flash 前必回读板上 `/tmp/fw.img` 字节数与本地比对, 不一致就清掉重推,
  **绝不把截断的镜像喂给 k7flash**(否则刷坏 uboot 槽要卡刷恢复)。
- **push 慢**: push 超时给到 600s, 不误判失败; 失败自动重试 `--retries` 次。
- **Ymodem 锁板(已根除自动触发)**: `--ymodem` 才会 `usbsw off`; 且失败时 finally
  里发 8×CAN 取消 rb、`usbsw on`, 把板子救回 nsh, 不再留死锁态。

## 1. 分层与复用
| 层 | 用什么 | 状态 |
|---|---|---|
| 传输 | **adb push(默认)** / 现成 `apps/system/ymodem` `rb`(备用) | 复用,不自研 |
| 临时存储 | **tmpfs** `/tmp/fw.img` | NuttX 现成,开 CONFIG_FS_TMPFS |
| 落盘+护栏 | **自研 `k7flash`** (nsh builtin) | 本架构核心 |
| 块写 | SDMMC host 写路径(sendsetup/CMD24) | 已上板验证 |
| 重启 | **`boardctl(BOARDIOC_RESET)`** → `board_reset` → PSCI SYSTEM_RESET | 已实现 |
| PC 自动化 | **自研 `k7_ota.py`**(adb push+校验+k7flash / 串口 Ymodem 备用) | 脚本 |

## 2. ★ 安全护栏 (硬性:绝不误刷 eMMC)
1. **物理隔离**:板上只实例化了 SDMMC host(`/dev/mmcsd0`=SD 卡),**eMMC host 从未实例化** → 代码物理上够不到 eMMC(0x2A330000)。这是最强护栏。
2. **目标写死**:`k7flash` 里目标块设备 = `"/dev/mmcsd0"` 常量,起始扇区 = `16384` 常量(uboot 槽)。**不接受设备/偏移命令行参数**,无默认全盘、无通配。指哪打哪。
3. **只碰 uboot 槽**:写范围 = sector 16384 起,长度 = 固件字节数(≤ trust 槽 24576 的距离,即 ≤4MB)。**校验不越界到 trust(24576)/rootfs**。绝不碰 idbloader(64)/GPT(0)。
4. **固件合法性**:写前校验 `/tmp/fw.img` 是合法 Rockchip FIT(U-Boot FIT magic `0xd00dfeed` @偏移0,或含我们 FIT 的特征串),拒绝乱文件。
5. **写后读回校验**:逐扇区读回比对,不一致则**不重启**并告警(避免半写砖卡)。
6. **失败不自动重启**:任何一步失败保持运行态,让用户可重试/串口介入。
7. **PC 端传输校验**:push 后板上大小回读比对,截断不落盘(见 §0.1)。

## 3. k7flash 命令 (自研核心)
```
k7flash <file>
  1. open(file) 读全部到堆 buffer(≤4MB)
  2. 校验: size>0 && size<=4MB && FIT magic(0xd00dfeed)
  3. find_blockdriver("/dev/mmcsd0") 取块设备(写死,不接受参数)
  4. 逐扇区 bops->write(buf, 16384, nsectors)  (nsectors=ceil(size/512))
  5. 逐扇区 bops->read 回读比对
  6. 全过 → boardctl(BOARDIOC_RESET); 否则报错保持运行
```
护栏都在源码写死,命令行只收 <file>,不收目标。

## 4. board_reset (PSCI 重启)
- `board_app_initialize` 同层实现 `board_reset(int status)`:调 arm64 PSCI `SYSTEM_RESET`(ARM64_HAVE_PSCI 已选)或 `up_systemreset`。
- 供 `boardctl(BOARDIOC_RESET)` 调用。

## 5. PC 端 k7_ota.py
默认(ADB, 推荐):
```
k7_ota.py --img out/uboot_nuttx.img [--port COMx] [--retries N]
  1. adb push uboot_nuttx.img /tmp/fw.img
  2. adb shell ls -l 回读板上字节数, 与本地比对; 不一致→清残file 重试(最多 N 次)
  3. 大小一致 → adb shell k7flash /tmp/fw.img(护栏落盘+校验+重启)
  4. 给了 --port 则刷完经串口拉起 adbd, 下轮继续走 ADB
```
备用(显式 --ymodem, 串口 1.5Mbaud, CH340 边际不稳):
```
k7_ota.py --img out/uboot_nuttx.img --port COMx --ymodem
  1. usbsw off + adb kill-server(让板下 USB 总线, 避免 host 流量搅坏传输)
  2. 串口触发 rb, Ymodem 发 FIT 到 /tmp/fw.img(失败自动 CAN 取消回 nsh)
  3. 串口触发 k7flash; 无论成败都把板子救回 nsh + usbsw on, 不锁死
```

## 6. 依赖 config
- `CONFIG_SYSTEM_YMODEM=y`(rb/sb 命令, 仅 --ymodem 用)
- `CONFIG_FS_TMPFS=y`(/tmp)
- `CONFIG_BOARDCTL_RESET=y`(boardctl reset)
- SDMMC 写路径(已有, 已验)
- 板上 adbd(DWC3 UDC + adb gadget, 默认通道用)

## 7. 分步落地
- S1 board_reset(PSCI) + BOARDCTL_RESET → 板上能 `reboot`。
- S2 k7flash 命令(护栏+落盘+校验),先不 reset 只写+校验验证。
- S3 接 ymodem(rb) + tmpfs,串口传 FIT 到 /tmp。
- S4 PC k7_ota.py 串联全流程。
- S5 端到端:改 nuttx → PC 一键 → 板上自更新 → 重启新固件。**不拔卡达成**。
- S6 健壮化: ADB 主通道 + 大小校验 + 重试; 去自动 Ymodem 回退(防锁板)。

## 8. 沉淀 skill
- 嵌入式 OTA 护栏三板斧:物理隔离(不实例化危险设备)+ 目标写死(无参数)+ 写后校验不砖。
- 复用 ymodem/adb 传输,自研只做"落盘+护栏",最小面积。
- PC 端稳健三条: 传输后回读校验(防截断)、失败重试给足超时、危险副作用(usbsw)只在显式请求时触发且带自愈。
