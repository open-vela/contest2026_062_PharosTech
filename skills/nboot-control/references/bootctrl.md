# bootctrl 元数据

N-Boot 的 A/B 状态存在磁盘上，位置是 GPT 分区 `bootctrl`。
它同时管理**两个互相独立的域**。

## 两个域

| 域 | 索引 | 载荷 | 谁来读 |
|---|---|---|---|
| NuttX | 0 | 裸 ARM64 Image（`0x40200000`） | `bootnuttx` |
| AMP | 1 | FIT（Linux + DTB + initramfs + openvela） | `bootamp` |

各域有自己的 `active_slot`、优先级、trial 计数、镜像大小与摘要。
改一个域不影响另一个。

## 磁盘布局

同一个 4096 字节记录存**两份**，位于分区起始 `+ 0` 和 `+ 4096`
（即 LBA `0x8000` 与 `0x8008`）。读取时取**合法的、generation 最大**的那份。

```
偏移    长度   字段
0       8      magic = "K7ABCTRL"
8       2      format_version = 1      (LE u16)
10      2      header_size = 20        (LE u16)
12      8      generation              (LE u64)
20      108    domains[0]  = NuttX 域
128     108    domains[1]  = AMP 域
236     4      一次性启动请求（见下）
240     3852   padding
4092    4      crc32 (LE u32)，覆盖 [0, 4092)
```

### 域（108 字节）

```
偏移   长度   字段
0      1      active_slot   0=A, 1=B（>1 为非法）
1      3      reserved
4      52     slots[0] = A 槽
56     52     slots[1] = B 槽
```

### 槽（52 字节）

```
偏移   长度   字段
0      1      priority        0 = 不可启动
1      1      tries_remaining
2      1      successful
3      1      reserved
4      8      image_size      (LE u64)
12     8      image_version   (LE u64)
20     32     sha256
```

## 槽位判据：优先级决定一切

```c
static bool k7_bootctrl_slot_bootable(const struct k7_slot_disk *slot)
{
	return slot->priority != 0;
}
```

**只看 `priority`。** `tries_remaining` 与 `successful` 不参与 NuttX 的启动判决，
它们是磁盘格式的历史遗留字段。

选择规则：优先级最高者胜，**平手时偏向 `active_slot`**。

镜像校验失败时，`priority` 与 `tries_remaining` 一起被清零，落到另一槽
（同一轮启动内完成，不重启）。

**因此**：一个被激活但从未启动过的槽也照常启动；一个早期固件耗尽了
retry 计数的槽也照常启动。

## 写入顺序

`k7_bootctrl_write()` 从不原地改。顺序是：

1. 递增 generation，重算 CRC
2. 先写**较旧**的那份副本
3. **回读校验**它合法且 generation 一致，不一致则 `-EIO`
4. 再写另一份（这份失败只打印
   `bootnuttx: warning: second bootctrl copy was not updated`，不中断）

这样任一时点断电，至少有一份副本是完整的。

## 一次性启动请求

`offset 236` 的 4 字节（原是 padding）：

```c
#define NBOOT_REBOOT_MAGIC      0x4e425200U
#define NBOOT_REBOOT_MAGIC_MASK 0xffffff00U
#define NBOOT_REBOOT_REQUEST(t) (NBOOT_REBOOT_MAGIC | ((t) & 0xffU))
```

写入走正常的两副本流程，带新的 generation 与 CRC。N-Boot 在**执行前**
就把它清零（也是正常写入流程），所以恰好消费一次。写入失败时请求**不消费**，
打印 `N-Boot: could not consume reboot request`。

格式版本仍是 1。旧版本忽略这几个 padding 字节，所以旧 N-Boot 不会误触发。

## 启动交接记录（N-Boot → OS）

N-Boot 在跳转前把本次启动的关键信息写进 PMU1 GRF 暂存寄存器，
供操作系统判断"我是从哪起来的"。

| 地址 | 方向 | 含义 |
|---|---|---|
| `0x26026230` | OS → N-Boot | 传统一次性请求（OS_REG12） |
| `0x26026234` | N-Boot → OS | 交接头 |
| `0x26026238` | N-Boot → OS | generation 低 32 位 |
| `0x2602623c` | N-Boot → OS | generation 高 32 位 |

交接头位域：

```
31:16   magic 0x4e48
15:12   handoff version = 2
11:8    reason: 0=正常, 1=请求的槽, 2=回退
7:4     medium: 1=SD, 2=eMMC
3:0     slot:   0=A, 1=B
```

**写入顺序**：先两个 generation 字，**最后**写交接头。这样读到有效头
就意味着 generation 已经就位。

每次启动开始时 N-Boot 会清掉旧的交接头。读者**必须**先校验 magic 与
version 再用其余字段。

## 主机侧工具

两个独立实现，格式一致（一份 GPL-2.0+ 在 N-Boot 仓，一份 Apache-2.0 在队伍仓）：

```sh
# N-Boot 仓
python3 tools/nboot/bootctrl.py init --output bootctrl.bin \
    --nuttx-a nuttx-a.bin --nuttx-b nuttx-b.bin \
    --amp-a amp-a.itb --amp-b amp-b.itb
python3 tools/nboot/bootctrl.py inspect bootctrl.bin

# 队伍仓（同格式）
python3 tools/k7_abpack/bootctrl.py init --output bootctrl.bin --nuttx-a nuttx.bin
```

`init` 默认值：A 槽 `priority=15`、B 槽 `priority=14`，`successful=1`，
`tries_remaining=0`。**未提供镜像的槽 `priority=0`**，即 AMP 域默认不可启动。

两份副本被写成**逐字节相同**、同 generation、同 CRC。

⚠️ **改格式时必须同步改这两处**，否则打包与 sync 会静默错配。
