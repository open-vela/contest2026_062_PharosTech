# N-Boot 命令与 OEM 子命令

## 两个构建 profile

```
kickpi-k7-rk3576_defconfig        最小：只 bootnuttx，无 USB 恢复
kickpi-k7-rk3576-full_defconfig   完整：bootamp → bootnuttx → fastboot
```

```sh
make CROSS_COMPILE=aarch64-linux-gnu- kickpi-k7-rk3576-full_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- -j4 u-boot-nodtb.bin u-boot.dtb
```

产物是 **proper + DTB 两件**，不是可刷镜像。4 MiB FIT 由队伍仓的
`tools/k7_pack/build_nboot_ab.sh` 从 rkbin 取 ATF/OP-TEE 组装。

## 启动链（full profile）

```
bootcmd = "if bootamp; then true; elif bootnuttx; then true; else fastboot usb 0; fi"
```

顺序固定：AMP 槽 → NuttX 槽 → USB fastboot。

**例外**：若存在一次性槽位请求（见下），`bootamp` 立刻返回失败，
把本次启动让给被请求的 NuttX 槽。

## 控制台命令

```
bootnuttx [mmc-device]             启动已校验的 NuttX A/B 槽
bootamp                            按活动槽冷启 AMP FIT
bootamp <addr> <size> [check]      RAM 试跑（地址/长度均为十六进制）
```

`bootamp ... check` **只做校验**：不加载载荷、不启动 CPU、不消耗 trial。
这是验证一个 AMP FIT 是否合法的最省事方式。通过时打印
`bootamp: FIT preflight passed; no CPU or payload changed`。
完整的 RAM 迭代回路、报错对照与精确契约见 [bootamp-ram.md](bootamp-ram.md)。

⚠️ **串口发命令只用单个 CR 结尾。** CRLF 里的 LF 会被当成空命令、重复执行
上一条，并且会把正在运行的 fastboot 立刻踢出去（实测确认，2026-09-10）。

`bootnuttx 1` 显式指定 eMMC，`bootnuttx 0` 指定 SD，不带参数走介质选择策略。

## fastboot

USB 身份：`18d1:d00d`。下载缓冲 64 MiB（`0x04000000`），溢出时 `stage` 直接
`cannot load`——大文件必须分块。

### 标准命令中被 N-Boot 改写的

| 命令 | 行为 |
|---|---|
| `flash nuttx_a` / `flash nuttx_b` | **拦截**，转到校验路径（回读 SHA-256 + 更新 bootctrl） |
| `flash amp_a` / `flash amp_b` | **拦截**，同上（旧版 N-Boot 会落到通用裸写） |
| `flash nboot` | **拦截**，走 N-Boot 自更新（校验 6 个子镜像后原位写 + 回读） |
| `flash <其他>` | 通用裸写，写任何 GPT 分区，**无校验** |
| `erase <name>` | 允许，无授权要求 |
| `boot` / `set_active` | **拒绝**，提示 `use verified board:activate:<slot>` |
| `stage` | 下载到 RAM 并返回 `OKAY`，供后续 `oem board:flash:<slot>` 消费 |

### ⚠️ `flash` 必须带文件名

host 侧 fastboot 的语法是 `fastboot flash <partition> [<file>]`。
**省略文件名不会回退到上次 `stage` 的数据**（那是另一个会话），
而是报：

```
unknown partition 'amp_a'
fastboot: error: cannot determine image filename for 'amp_a'
```

**这个报错有误导性** —— 它看起来像"分区不存在"，实际是"缺第二个参数"。
分区明明存在（`getvar partition-size:amp_a` 会正常返回）。

正确用法是**两条**：

```sh
fastboot flash amp_a amp-shmem.itb          # 一条命令里带文件名
```

或先 `stage` 再用 `oem board:flash:`：

```sh
fastboot stage amp-shmem.itb
fastboot oem board:flash:amp_a              # OEM 路径吃 stage 的缓冲
```

### OEM 子命令

```
fastboot oem board:target:auto|sd|emmc       设定本次会话的写入介质
fastboot oem board:flash:<slot>              校验写入槽载荷
fastboot oem board:activate:<slot>           校验后激活
fastboot oem board:flash:nboot               更新 N-Boot 自身
fastboot getvar nboot-medium                 查询当前解析到的介质
```

`<slot>` 允许值：`nuttx_a`、`nuttx_b`、`amp_a`、`amp_b`。
其他值 → `FAIL partition is not in recovery allowlist`。

**`target` 只影响本次 USB 会话**，拔线即复位为 `auto`。它不写介质。

### 常被搞混的两点

1. `fastboot flash amp_a` **不等于** `fastboot oem board:flash:amp_a`。
   前者是通用裸写，后者才更新 bootctrl。AMP 必须用后者。
2. `fastboot flash <part>` 与 `fastboot oem board:flash:<slot>` 也是不同路径。
   前者裸写不写元数据，后者写。

### 退出

fastboot 收到**任意键**即退出（`CONFIG_CMD_FASTBOOT_ABORT_KEYED`），
打印 `\rOperation aborted.\n`。此前 `bootdelay` 已被置 `-1`，
所以不会顺势自动启动，停在 `N-Boot>`。

脚本里退出 fastboot 的惯用做法是发**单个 ETX（0x03）**（历次 `bootamp` RAM
试跑均如此，实测确认）。反过来：`bootcmd` 落到 fastboot 时，串口上任何杂散
输入都会让它退出并打印 `Operation aborted.`——看到这行不代表有人按了键，
见 [known-issues.md](known-issues.md) #11。

## getvar 变量

标准项：`version`、`version-bootloader`、`downloadsize`、`max-download-size`、
`serialno`、`version-baseband`、`product`、`platform`、`current-slot`、
`is-userspace`。

**`nboot-medium`** 是 N-Boot 唯一新增项，通过环境变量
`fastboot.nboot-medium` 实现（`getvar` 先查 `fastboot.<name>` 环境变量）。
它不出现在 `getvar all` 里。

⚠️ `current-slot` **硬编码为 `"a"`**，与 bootctrl 无关，不可用作槽位判据。
真实槽位从 PMU1 GRF 交接记录读，见 [bootctrl.md](bootctrl.md)。

## 串口恢复

```
CONFIG_AUTOBOOT_STOP_STR="!"
CONFIG_BOOTDELAY=0
```

板子在 bootdelay 期间**有自己的 100 ms 轮询**（10 次 × 10 ms，每次排空
输入缓冲），首字符 `!` 命中即进控制台并打印
`N-Boot: serial recovery requested, entering console`。

之所以不用 U-Boot 通用的 keyed-autoboot 解析器：`bootdelay=0` 时它的
输入窗口实际上是空的。

**使用方法**：先启动发送脚本，**再**复位板子。必须在复位前后持续发 `!`。

```sh
python3 tools/nboot/request_recovery.py --port /dev/ttyUSB0
```

⚠️ 该脚本超时也返回 0，退出码不可判失败。要盯串口输出里的
`serial recovery requested` 或 `N-Boot>` 提示符。

**必须单进程独占串口。** 分成两个进程会互相抢端口，`!` 全被其中一方吃掉。

## 硬件恢复键

SARADC `adc@2ae00000` 通道 1，`value < 100` 判为按下。命中则打印
`N-Boot: recovery key pressed, entering Fastboot` 并直接进 Fastboot。

## 一次性启动请求

两种来源，**持久请求优先**：

| 来源 | 位置 | 可靠度 |
|---|---|---|
| 持久请求 | bootctrl 记录 offset 236 的 4 字节 | 实测可靠 |
| 传统寄存器 | PMU1 OS_REG12 `0x26026230` | 实测复位链上不可靠 |

值 = `0x4e425200 \| target`：

| target | 行为 |
|---|---|
| 1 | 停在 `N-Boot>` 控制台 |
| 2 | 进 USB Fastboot |
| 3 | 本次强制 NuttX 槽 A |
| 4 | 本次强制 NuttX 槽 B |

无效值被消费后忽略。槽位请求只影响一次启动，**不改变** `active_slot`。
请求在被执行前就已清除（用新的 generation + CRC 写回），所以恰好生效一次。

从 NuttX 侧发请求用 `nbootctl reboot`（走持久请求），见 [nbootctl.md](nbootctl.md)。

## NuttX 侧 `nbootctl` 动词一览

完整说明在 [nbootctl.md](nbootctl.md)；这里只列名字与所在的树。

| 动词 | product 树 | 草稿树 |
|---|:---:|:---:|
| `status` `verify` `set-active` `mark-successful` | 有 | 有 |
| `stage`（写非运行槽 + 回读 + **激活**）`clone` `update-nboot` | 有 | 有 |
| `reboot console\|fastboot\|nuttx-a\|nuttx-b` | 有 | 有 |
| `digest` `verify-part` `write-part` `write-raw` `write-gpt` `check-raw` `format` | **无** | 有（未上板） |
| 库：`nbootctl_handoff_read()` `nbootctl_bootctrl_snapshot()` | 有 | **无** |

product 树 = `tmp/product-wt/app/nbootctl/`；
草稿树 = `tmp/amp-compute-draft-20260914/app/nbootctl/`。
