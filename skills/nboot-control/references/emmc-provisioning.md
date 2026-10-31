# 给空白盘装系统（eMMC 从零起步）

适用场景：目标介质上没有可用的分区表与 loader。典型是全新的 eMMC，
或一块被清过的盘。

## 为什么常规路径走不通

三条限制叠在一起：

1. **`fastboot oem board:flash:*` 与 `flash nuttx_a|b` 要求布局已存在。**
   `nboot_storage_check_layout()` 逐项比对 `uboot`/`trust`/`bootctrl`/
   `nuttx_a`/`nuttx_b` 的**名称、起始扇区、扇区数**。空白盘无分区表 →
   一律 `FAIL invalid recovery partition layout`。

2. **MiniLoader 在 LBA 64，不属于任何 GPT 分区。**
   GPT 的保护性 MBR 占 LBA 0，主表占 LBA 1–33，第一个分区通常从更后面开始。
   `fastboot flash <part>` 按分区名查找，够不到 LBA 64。

3. **rockusb 通道不通。**
   N-Boot 的 USB gadget 用 `CONFIG_USB_GADGET_VENDOR_NUM=0x18d1` /
   `PRODUCT_NUM=0xd00d`。Rockchip 的 `upgrade_tool` / `RKDevTool` 按
   `VID 0x2207` 过滤，永远看不到这颗设备（实测 `List of rockusb connected(0)`）。

## 解法：stage 到 RAM，板内 mmc write

```
fastboot stage FILE              → 下载到 0x60000000，不要求目标盘有分区表
（发任意键退出 fastboot）
mmc write 0x60000000 <lba> <cnt> → 板内写到任意 LBA，含不属于任何分区的 LBA 64
crc32 0x60000000 <bytes>         → 板子自己算内存 CRC32
```

退出 fastboot 后 **RAM 内容保留**，所以"先下载、后写盘"两步可以分开做。
`crc32` 是 U-Boot 的纯内存 CRC32，与主机 `zlib.crc32` 逐字节同源，
可以做**全量**校验而非抽样。

工具：[../scripts/nboot_flash.py](../scripts/nboot_flash.py)

```sh
python3 nboot_flash.py --port COM11 --dev 1 write 0x40   MiniLoaderAll.bin
python3 nboot_flash.py --port COM11 --dev 1 write 0x4000 uboot.img
python3 nboot_flash.py --port COM11 --dev 1 gpt layout.txt
```

它每步都做：stage → RAM CRC32 比对 → `mmc write` →
**从介质回读再算 CRC32** 比对。三处一致才算过。

## 写 GPT

`gpt write` 的布局串只能作为命令字面量传入，而 1.5 Mbaud 的 CH340 链路会
**丢字符**（实测 `0x100000` 变成 `0100000`）。所以先 `setenv`，
**回读 `printenv` 逐字符比对**，确认板子收到的就是完整那一串，再执行。
不回读就写分区表等于闭眼改分区。

⚠️ **`gpt write` 的 `start` 与 `size` 要字节数，不是扇区数。**
`set_gpt_info()` 里对两者都做 `lldiv(..., dev_desc->blksz)`。
文档没写这一点。传扇区数不会报错，只会得到一堆小得离谱的分区，
然后 `gpt_restore` 失败打印 `error!`。

```
uuid_disk=<32位十六进制>;
name=uboot,start=0x800000,size=0x400000;
name=trust,start=0xc00000,size=0x400000;
name=bootctrl,start=0x1000000,size=0x100000;
name=nuttx_a,start=0x1200000,size=0x4000000;
name=nuttx_b,start=0x5200000,size=0x4000000;
name=amp_a,start=0x9200000,size=0x20000000;
name=amp_b,start=0x29200000,size=0x20000000;
name=data,start=0x49200000,size=<剩余字节>
```

（上面每项都是字节数；除以 512 即得下表扇区。）

### GPT 结构要点

`gpt write` 产出的盘上有：

```
LBA 0        保护性 MBR（全零引导代码 + 一个 0xEE 类型的分区项）
LBA 1        GPT 主表头（magic "EFI PART"）
LBA 2..33    128 个分区项，每项 128 字节
```

**解析时注意两点**：

1. **表头在 LBA 1，不是 LBA 0。** LBA 0 是保护性 MBR，前 8 字节通常是
   全零。按 offset 0 找 `EFI PART` 会失败。
   （`dd` 出来的分区区域可能正好从表头开始，所以两种偏移都要试。）

2. **空槽的 type GUID 是全零。** 分区项的 type GUID 为 0 表示该条目未使用，
   即使其 name 字段有残留字节。判断"这个分区存在"要看 type GUID，
   不能只看 name。

`check_bootctrl.py --layout` 同时处理了这两点：

```sh
python3 scripts/check_bootctrl.py --layout disk.img
```

正确布局的输出：

```
  uboot        start=0x4000     blocks=8192
  trust        start=0x6000     blocks=8192
  bootctrl     start=0x8000     blocks=2048
  nuttx_a      start=0x9000     blocks=131072
  nuttx_b      start=0x29000    blocks=131072
  ...
layout matches the bootloader's expectation
```

不匹配时逐条列出问题（例如旧版布局缺 `bootctrl`/`nuttx_a`/`nuttx_b`），
退出码 1。**这个检查能在写盘前发现"N-Boot 根本不会选中这块盘"。**

## 标准布局

扇区号（512 字节），与 `include/nboot_storage.h` 及队伍仓
`tools/k7_pack/build_nboot_ab.sh` 一致：

| 分区 | 起始扇区 | 扇区数 | 大小 |
|---|---:|---:|---:|
| `uboot` | 16384 (`0x4000`) | 8192 | 4 MiB |
| `trust` | 24576 (`0x6000`) | 8192 | 4 MiB |
| `bootctrl` | 32768 (`0x8000`) | 2048 | 1 MiB |
| `nuttx_a` | 36864 (`0x9000`) | 131072 | 64 MiB |
| `nuttx_b` | 167936 (`0x29000`) | 131072 | 64 MiB |
| `amp_a` | 299008 (`0x49000`) | 1048576 | 512 MiB |
| `amp_b` | 1347584 (`0x149000`) | 1048576 | 512 MiB |
| `data` | 2396160 (`0x249000`) | 剩余 | — |

**分区外**的内容（`gpt write` 不覆盖，必须单独写）：

| 内容 | LBA | 说明 |
|---|---:|---|
| MiniLoaderAll.bin | 64 | BootROM 的 loader。**不在任何分区内** |
| GPT 主表 | 1–33 | `gpt write` 自己写 |

## 完整步骤

```sh
# 1. loader（分区外，先写，否则板子起不来）
nboot_flash.py --dev 1 write 0x40  MiniLoaderAll.bin    # 360448 B

# 2. 分区表
nboot_flash.py --dev 1 gpt layout.txt

# 3. 引导链
nboot_flash.py --dev 1 write 0x4000 uboot.img           # 4 MiB FIT
nboot_flash.py --dev 1 write 0x6000 trust.img           # 4 MiB

# 4. bootctrl 元数据
nboot_flash.py --dev 1 write 0x8000 bootctrl.img

# 5. 主域固件（双槽同一镜像）
nboot_flash.py --dev 1 write 0x9000  nuttx_a.img
nboot_flash.py --dev 1 write 0x29000 nuttx_b.img

# 6. 数据分区（可大于 64 MiB，自动分块）
nboot_flash.py --dev 1 write 0x249000 data.img
```

## 分块

`CONFIG_FASTBOOT_BUF_SIZE` 是 `0x04000000`（64 MiB）。超过的 `stage` 直接
`cannot load`。`nboot_flash.py` 自动按 `--chunk-bytes`（默认 60 MiB）切块，
每块独立校验。

## 路径必须是纯 ASCII

fastboot 的子进程链路过不了非 ASCII 路径：实测中文目录名会变成 `????`
然后 `cannot load`。工具把分块文件先落到
`%TEMP%\nboot-stage\`（纯 ASCII），再传给 fastboot。

## 中文路径命令行的另一个坑

Git Bash 里的中文路径在某些 Python/subprocess 组合下会被破坏成 `????`。
写入前用 `os.path.getsize()` 能打开就说明路径没问题；打不开就是这个问题。

## 验证

```sh
# 读回某个 LBA 与主机文件比对
nboot_flash.py --dev 1 verify 0x4000 uboot.img

# 或板内直接算
mmc dev 1; mmc read 0x52000000 0x4000 0x2000; crc32 0x52000000 0x400000
```

## 收尾：让新内容生效

**空白盘装完后必须冷启动。** 板子内存里跑的还是旧的 N-Boot
（或压根没起来）。`version` 显示的是**内存中正在运行**的镜像，
不是盘上的。

冷启动前务必：**移除非目标介质**。SD 卡在槽里且布局有效 → 永远从 SD 起，
eMMC 上的新系统不会被加载。见 [recovery.md](recovery.md)。
