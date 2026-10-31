# nbootctl — NuttX 侧的 N-Boot 控制工具

位置：`app/nbootctl/`（openvela/NuttX 应用）。

开发时的刷写走 fastboot。它的本职是两件事：**读 N-Boot 留下的启动交接
记录**，以及**向 N-Boot 发一次性启动请求**。此外它能在系统运行中写槽
（`stage`），产品的无线升级就建在这上面，见 [ota.md](ota.md)。

有两棵树带这个工具，动词集合不同——下面每组都标了在哪棵树：

| 树 | 路径 | 独有的东西 |
|---|---|---|
| product | `tmp/product-wt/app/nbootctl/` | 结构体版读接口（库） |
| 草稿 | `tmp/amp-compute-draft-20260914/app/nbootctl/` | 分区级动词 `write-part` 等 |

## 为什么需要它

NuttX 启动后无法从分区表判断"我是从哪个槽起来的"——`bootnuttx` 可能走了
请求的槽、也可能走了回退。bootctrl 里的 `active_slot` 也不等于本次实际
启动的槽。

N-Boot 在跳转前把这次的事实写进 PMU1 GRF 暂存寄存器，
`nbootctl status` 就是读这些寄存器。

## 命令

```
nbootctl status                              读启动交接记录
nbootctl verify          nuttx|amp a|b       按记录的尺寸与摘要校验介质
nbootctl set-active      nuttx|amp a|b       改 active_slot
nbootctl mark-successful nuttx|amp a|b       标记该槽已确认
nbootctl stage           nuttx|amp IMAGE     写非运行槽 + 回读 SHA-256 + 记元数据 + 激活
nbootctl clone           nuttx|amp a|b a|b   复制一个已验证槽到另一个（不激活）
nbootctl update-nboot    IMAGE               更新 N-Boot 自身（写后回读，不自动重启）
nbootctl reboot          console|fastboot|nuttx-a|nuttx-b
```

以上八个动词在 **product 树与草稿树都有**（`tmp/product-wt/app/nbootctl/`、
`tmp/amp-compute-draft-20260914/app/nbootctl/`）。

⚠️ **`stage` 写完即激活**，与 fastboot 的"flash 后另行 activate"不同。
NuttX 域的目标是**非运行槽**（按交接记录），AMP 域的目标是非 `active_slot`。
完整语义与 OTA 用法见 [ota.md](ota.md)。

### 分区级动词（**仅草稿树**，未移植到 product）

```
nbootctl digest       FILE                        打印 "<字节数> <sha256>"
nbootctl verify-part  FILE SHA256                 只比对文件摘要，不写
nbootctl write-part   PARTITION FILE SHA256       写命名分区
nbootctl write-raw    LBA SECTORS FILE SHA256     写绝对 LBA（SECTORS 是容量上限）
nbootctl write-gpt    FILE SHA256                 写 LBA 0 起最多 34 扇区
nbootctl check-raw    LBA SECTORS SHA256          只读：介质区间的摘要比对
nbootctl format       PARTITION                   mkfatfs，卷标 "NYABULA"
```

读码确认；实测日志里**没有这些动词的上板记录**。来源：草稿树
`nbootctl_part.c`、`nbootctl_format.c`、`nbootctl_main.c`。

- **摘要是必填的，没有"不带摘要的写"。** 每次写做两道比对：源文件 vs 期望摘要
  （不符报 `source digest mismatch`，`-EKEYREJECTED`，**此时还没 `open()` 介质**）；
  写后回读 vs 期望摘要。成功输出以 `readback OK` 结尾。
- 装不下：`payload does not fit (N > M blocks)`，`-EFBIG`，未动介质。
- `SHA256` 是 64 位十六进制，大小写均可。
- ⚠️ 读码发现一处疑似缺陷：文件长度非 512 整数倍时回读比对预计恒失败
  （未上板复现）。移植前先看 [known-issues.md](known-issues.md) #14。
- `write-part` 经**分区自己的设备节点**（`/dev/mmcsdNpM`）写，由 NuttX 把写入
  限在分区内；`write-raw`/`write-gpt`/`check-raw` 走整盘节点
  （`/dev/mmcsd0`=SD，`/dev/mmcsd1`=eMMC，介质取自交接记录）。
- `write-raw` 是为分区之外的区域准备的——主要是 LBA 64 的 MiniLoader。
- `format` 不检查分区是否已挂载。对挂载中的分区 mkfatfs 结果未定义，
  调用者自己负责。草稿树还提供 `kickpi_k7_storage_format_hook()` 的强定义：
  板级存储驱动开机发现从未格式化的分区时会经它自动格式化。

分区白名单（名字 → GPT 索引 → 容量上限，单位 512 B 块）：

| 名字 | 索引 | 块数 | | 名字 | 索引 | 块数 |
|---|---:|---:|---|---|---:|---:|
| `uboot` | 1 | 8192 | | `amp_a` | 6 | 1048576 |
| `trust` | 2 | 8192 | | `amp_b` | 7 | 1048576 |
| `bootctrl` | 3 | 2048 | | `config` | 8 | 65536 |
| `nuttx_a` | 4 | 131072 | | `data` | 9 | 0 = 到盘尾，不限 |
| `nuttx_b` | 5 | 131072 | | | | |

不在表里的名字：`unknown partition <name>`，`-EINVAL`。草稿树的 usage 文本与
`DISTRIBUTION.md` 只列了 8 个（漏 `config`），以代码里的表为准。

**没有硬护栏**：白名单里有 `uboot`、`trust`、`bootctrl`，`write-gpt` 能换分区表。
这是设计取舍（`DISTRIBUTION.md`：板子有 MaskROM/Loader 可救，硬护栏只会挡住
正当操作），防护靠摘要、容量检查、回读与上层的二次确认。用这些动词前确认介质
——它们写的是**本次启动所在的那块盘**。

### `status` 输出

```
medium=emmc
slot=a
generation=4
reason=normal
bootctrl_generation=4
nuttx_active=a
nuttx_a priority=15 successful=1 size=4752792 version=1
nuttx_b priority=0 successful=0 size=4752792 version=1
amp_active=a
amp_a priority=15 successful=1 size=0 version=0
amp_b priority=0 successful=0 size=0 version=0
```

- `medium`：`sd` / `emmc`
- `slot`：本次实际启动的槽
- `reason`：`normal` / `requested-slot` / `fallback`
- 后面每行是 bootctrl 里各槽的原始状态

### `reboot` 的目标

| 参数 | 效果 |
|---|---|
| `console` | 下次启动停在 `N-Boot>` |
| `fastboot` | 下次启动进 USB Fastboot |
| `nuttx-a` / `nuttx-b` | 本次强制该槽，不改 `active_slot` |

实现（读码确认，product 树与草稿树一致）：先读交接记录得到介质，
`nbootctl_bootctrl_request(medium, target)` 把 `0x4e425200 | target` 写进
bootctrl 记录 **offset 236 的持久请求**（走正常的两副本写入与回读），打印
`nbootctl: one-shot target N stored`，`dsb sy` 后 `boardctl(BOARDIOC_RESET, 0)`。
复位若意外返回，会把请求清回 0 并报 `reset returned unexpectedly`。

实测（2026-09-10）：`nbootctl reboot console` 后 N-Boot 打印
`N-Boot: one-shot request 1` 并停在控制台。

> 本页早先写的是"`nbootctl reboot` 写 PMU1 OS_REG12"，与当前两棵树的源码都不符，
> 已更正。OS_REG12 在实测的复位链上不可靠（README：did not survive the tested
> loader reset chain reliably），工具走的是持久请求。N-Boot 侧仍然认 OS_REG12
> （见 [commands.md](commands.md)「一次性启动请求」），但不要依赖它。

没有有效交接记录（镜像不是 N-Boot 启动的）时 `reboot` 直接失败：
不知道介质就不知道该把请求写进哪块盘的 bootctrl。

## 交接头解析

```c
#define NBOOTCTL_HANDOFF_REG        0x26026234ul
#define NBOOTCTL_HANDOFF_MAGIC      0x4e480000u
#define NBOOTCTL_HANDOFF_MAGIC_MASK 0xffff0000u
#define NBOOTCTL_HANDOFF_VERSION    2u
```

读取时**先校验 magic 与 version**，再取字段：

```
reason = (h >> 8) & 0xf     0=normal, 1=requested-slot, 2=fallback
medium = (h >> 4) & 0xf     1=sd, 2=emmc
slot   = h & 0xf            0=a, 1=b
```

工具会**把 header 读两次**以确认不是半更新状态（N-Boot 最后才写 header，
所以读到有效 header 时其余字段已就位）。

## 当库用：结构体版读接口（**仅 product 树**）

`nbootctl_bootctrl.c` 同时是个小库。要在别的代码里报告槽位状态，
**不要解析 `nbootctl status` 的文本**，用：

```c
int nbootctl_handoff_read(unsigned int *medium, unsigned int *slot,
                          unsigned int *reason, uint64_t *generation);
int nbootctl_bootctrl_snapshot(struct nbootctl_state_s *state);
```

两者都不打印。`tries_remaining` 故意不在返回的结构体里（N-Boot 不读它）。
返回值、结构体定义与"每个镜像只能链接一次"的约束见 [ota.md](ota.md)
「结构体版读接口」。使用者：Nyabula Core 的 `update.status`。

## Kconfig

```
CONFIG_SYSTEM_NBOOTCTL=y
    depends on BOARDCTL_RESET
    select CRYPTO
```

配套：`_PROGNAME`("nbootctl")、`_PRIORITY`(100)、`_STACKSIZE`(4096)。

## 写入语义

`stage` / `set-active` / `mark-successful` 都通过 `nbootctl_write_records()`
写 bootctrl，顺序与 N-Boot 侧一致：

1. 先写**较旧**的副本
2. 回读校验 CRC 与内容
3. 再写另一份
4. 两份逐字节一致

**任一步失败即中止**，且选中副本保持原样（不会被半写坏）。
`tools/tests/test_nbootctl_records.py` 覆盖了故障注入：
首写 `-EIO`、回读被翻转、两次回读都坏——都要求选中副本仍是原来的
`0x5a` 填充、返回错误。

## 前置条件

- 分区布局固定：`bootctrl` 是分区 3，NuttX A/B 是 4/5，AMP A/B 是 6/7。
  **不支持任意布局。** 当前 eMMC 是 9 分区（2026-09-19 实测重建）：
  第 8 个 `config`（32 MiB）、第 9 个 `data`；前 7 个位置不变。
- AMP 域的操作**不与正在运行的 Linux 协调**（README 原话）。AMP 在跑的时候
  别从 openvela 侧写 AMP 槽。
- N-Boot 固定占 sector 16384 起 4 MiB。
- 需要支持**持久请求**的 N-Boot 版本（即当前版本）。

## 与 fastboot 的分工

| 任务 | 用哪个 |
|---|---|
| 从主机刷固件 | fastboot（`fastboot flash nuttx_a` 等） |
| 在设备上切换活动槽 | `nbootctl set-active` |
| 确认 AMP 槽 | `nbootctl mark-successful amp a` |
| 读本次启动的介质/槽 | `nbootctl status` |
| 重启到指定目标 | `nbootctl reboot console` |
| 更新 N-Boot 自身 | 两者都行；主机侧 `fastboot flash nboot` 更方便 |
| 无线升级（系统在跑、没接 USB） | `nbootctl stage` / 库函数，见 [ota.md](ota.md) |

`nbootctl stage` 能从板上文件刷固件，但设备上要有个完好的镜像文件
（`/data` 或 `/tmp`）。**"完好"要用摘要证明，不是"传完了"**——
2026-09-19 有过 scp 断线留下截断文件、没比哈希就刷进分区的事故。
开发时常规路径还是从主机 fastboot；产品的 OTA 走 `stage`。
