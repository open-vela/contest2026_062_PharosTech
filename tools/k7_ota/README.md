# KICKPI-K7 板上固件热更新 架构

> 目标:板上运行的 NuttX 接收新固件 → 写 SD 卡 uboot 槽 → 重启生效。**不拔卡**。
> 置信:设计(未上板)。写路径(SDMMC 写)改了 DMABEFOREWRITE 待上板验(sd_wr4.img)。

## 0. 一图
```
PC 端                              板上 (NuttX)
--------                           -----------------
build_sd 出 uboot_nuttx.img(FIT)
  │ 串口 Ymodem (sb / k7_ota.py)
  └───────────────────────────────► rb 收到 /tmp/fw.img (tmpfs)
                                       │
                                     k7flash /tmp/fw.img   ← 自研核心
                                       ├ 校验: FIT magic + 大小
                                       ├ 护栏: 目标写死 /dev/mmcsd0 @sector 16384
                                       ├ 写入 (SDMMC host 写路径)
                                       ├ 读回逐扇区校验
                                       └ boardctl(BOARDIOC_RESET) → PSCI 重启
                                                                     └► 新固件启动
```

## 1. 分层与复用
| 层 | 用什么 | 状态 |
|---|---|---|
| 传输 | **现成 `apps/system/ymodem`**(`rb` 收文件到 tmpfs) | 复用,不自研 |
| 临时存储 | **tmpfs** `/tmp/fw.img` | NuttX 现成,开 CONFIG_FS_TMPFS |
| 落盘+护栏 | **自研 `k7flash`** (nsh builtin) | 本架构核心 |
| 块写 | SDMMC host 写路径(sendsetup/CMD24) | 刚修 DMABEFOREWRITE,待上板验 |
| 重启 | **`boardctl(BOARDIOC_RESET)`** → `board_reset` → PSCI SYSTEM_RESET | 需实现 board_reset |
| PC 自动化 | **自研 `k7_ota.py`** (串口触发 rb + ymodem 发 + 触发 k7flash) | 脚本 |

## 2. ★ 安全护栏 (硬性:绝不误刷 eMMC)
1. **物理隔离**:板上只实例化了 SDMMC host(`/dev/mmcsd0`=SD 卡),**eMMC host 从未实例化** → 代码物理上够不到 eMMC(0x2A330000)。这是最强护栏。
2. **目标写死**:`k7flash` 里目标块设备 = `"/dev/mmcsd0"` 常量,起始扇区 = `16384` 常量(uboot 槽)。**不接受设备/偏移命令行参数**,无默认全盘、无通配。指哪打哪。
3. **只碰 uboot 槽**:写范围 = sector 16384 起,长度 = 固件字节数(≤ trust 槽 24576 的距离,即 ≤4MB)。**校验不越界到 trust(24576)/rootfs**。绝不碰 idbloader(64)/GPT(0)。
4. **固件合法性**:写前校验 `/tmp/fw.img` 是合法 Rockchip FIT(U-Boot FIT magic `0xd00dfeed` @偏移0,或含我们 FIT 的特征串),拒绝乱文件。
5. **写后读回校验**:逐扇区读回比对,不一致则**不重启**并告警(避免半写砖卡)。
6. **失败不自动重启**:任何一步失败保持运行态,让用户可重试/串口介入。

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
```
k7_ota.py --port COM3 --img out/uboot_nuttx.img
  1. 串口发 "rb\r\n"(启动板上 ymodem 接收到 /tmp/fw.img)  # 或 rb -f /tmp/fw.img
  2. Ymodem 协议发 uboot_nuttx.img
  3. 串口发 "k7flash /tmp/fw.img\r\n"
  4. 读板上校验输出 + 等重启日志确认
```

## 6. 依赖 config
- `CONFIG_SYSTEM_YMODEM=y`(rb/sb 命令)
- `CONFIG_FS_TMPFS=y`(/tmp)
- `CONFIG_BOARDCTL_RESET=y`(boardctl reset)
- SDMMC 写路径(已有,待验)

## 7. 分步落地
- S1 board_reset(PSCI) + BOARDCTL_RESET → 板上能 `reboot`。
- S2 k7flash 命令(护栏+落盘+校验),先不 reset 只写+校验验证。
- S3 接 ymodem(rb) + tmpfs,串口传 FIT 到 /tmp。
- S4 PC k7_ota.py 串联全流程。
- S5 端到端:改 nuttx → PC 一键 → 板上自更新 → 重启新固件。**不拔卡达成**。

## 8. 沉淀 skill
- 嵌入式 OTA 护栏三板斧:物理隔离(不实例化危险设备)+ 目标写死(无参数)+ 写后校验不砖。
- 复用 ymodem 传输,自研只做"落盘+护栏",最小面积。
