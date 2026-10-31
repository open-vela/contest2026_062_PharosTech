# 镜像制作与刷写

## 产物形状

| 载荷 | 形状 | 入口 | 目标槽 |
|---|---|---|---|
| NuttX 主域固件 | 裸 ARM64 Image，`ARMd` 在偏移 56 | `0x40200000` | `nuttx_a` / `nuttx_b` |
| AMP FIT | FIT，`d00dfeed` 魔数 | Linux `0x42000000`，openvela `0x4a400000` | `amp_a` / `amp_b` |
| N-Boot 自身 | 4 MiB 供应商 FIT（6 个子镜像） | `0x40200000` | `uboot` 分区 |

**形状由域决定，写错域会被拒。** `k7_recovery_flash()` 按域校验：

```c
if (slot_domain == K7_AMP_DOMAIN) {
        /* AMP：FIT 魔数 */
        if (size < 8 || memcmp(data, "\xd0\x0d\xfe\xed", 4))
                return -ENOEXEC;
} else {
        /* NuttX：ARM64 Image 头 */
        if (size < 60 || memcmp(data + 56, "ARMd", 4))
                return -ENOEXEC;
}
```

两边都接受会让错配的载荷先写进去、启动时才失败，且没有记录说明
写进去的是什么。

## AMP FIT 契约

外部数据 FIT（子镜像走 `data-position`/`data-size`，不用内联 `data`），
`/images` **恰好 4 个**子节点：

| 名称 | 载入地址 | 大小上限 | 类型 | 压缩 | 额外要求 |
|---|---|---:|---|---|---|
| `linux` | `0x42000000` | 80 MiB | kernel | none | `entry==load`、`cpu=0x100`、`os="linux"`、`ARMd`@56 |
| `fdt` | `0x4f000000` | 16 MiB | flat_dt | none | 必须通过 DTB 契约校验 |
| `ramdisk` | `0x50000000` | 256 MiB | ramdisk | **gzip** | `os="linux"` |
| `openvela` | `0x4a400000` | 16 MiB | firmware | none | `entry==load`、`cpu=0`、`ARMd`@56 |

`/configurations/conf` 必须：

```
nyabula,amp-abi = 2
kernel    = "linux"
fdt       = "fdt"
ramdisk   = "ramdisk"
loadables = "openvela"      ← 恰好一个
（不得有 "firmware" 属性）
```

每个子镜像都要有 `hash` 子节点，`algo="sha256"`、32 字节 `value`、
**不得有 `ignore`**。全部 8 字节对齐、互不重叠。

`linux` 与 `openvela` 的"大小上限"同时卡两样：文件长度，以及 Image 头偏移 16
的 **`runtime_size`**（须 ≥ 文件长度且 ≤ 上限）。BSS 大的镜像文件小也可能超标。
精确判据见 [bootamp-ram.md](bootamp-ram.md)。

### 引导 DTB 契约

- `/cpus`：**只有 4 个 A72 节点**，affinity 必须在 `0x100..0x103`，
  `enable-method="psci"`、`compatible="arm,cortex-a72"`。
- `/reserved-memory`：`#address-cells=2`、`#size-cells=2`、`ranges` **存在且为空**。
- `/serial@2ad40000` 状态必须是 `disabled`；所有 `rockchip,fiq-debugger` 也是。
- 恰好一个 `rockchip,amp` 节点，`status="okay"`。
- `amp-irqs`：长度是 6 个 u32 的整数倍，**至少 2 条路由**，上限 16 条。
  每条 `[0]=0`、`[2]=0`、`[3]=0x80`（优先级）、`[4]=0`、`[5]=0`。
  必须**包含** IRQ 108（UART）与 174（MAILBOX）；额外的合法路由被接受
  （音频需要 SAI1/I2C3/DMA 路由到这里）。
- 四个 carveout，各自 `no-map`，地址与大小必须精确：

| 路径 | 基址 | 大小 |
|---|---|---|
| `rpmsg@47800000` | `0x47800000` | 2 MiB |
| `rpmsg-dma@47a00000` | `0x47a00000` | 2 MiB |
| `amp-shmem@47c00000` | `0x47c00000` | 4 MiB |
| `openvela@4a400000` | `0x4a400000` | 16 MiB |

校验只按路径查这四块；**额外的 reserved-memory 节点不被检查也不被拒绝**
（读码确认）。完整 product 固件就是靠这一点、在不改 N-Boot 的前提下放进 AMP 的，
做法与三个失败签名见 [bootamp-ram.md](bootamp-ram.md)。

## AMP 试跑不再需要写盘

```
fastboot stage amp.itb
（发任意键退出 fastboot）
bootamp 60000000 <hex-size> check      ← 只校验，不加载、不起 CPU、不消耗 trial
bootamp 60000000 <hex-size>            ← 真跑
```

`check` 是最省的验证手段：一个不合法的 FIT 会在写盘前就被指出来。
串口行尾、退出 fastboot 的方式、各条报错的含义见
[bootamp-ram.md](bootamp-ram.md)。

⚠️ 一旦真跑，**失败即复位整个 SoC**（Linux 起来之后的任何超时都会
`reset_cpu()`）。这不是看门狗：已确认过的内核之后崩了，仍要手动恢复。

## 写主域固件

```sh
fastboot stage nuttx.bin
fastboot oem board:flash:nuttx_a       # 或 fastboot flash nuttx_a
fastboot oem board:activate:nuttx_a
```

`flash` 的完整路径：

1. 尺寸检查（非零，且不超过分区容量）
2. **按域校验载荷形状**
3. 计算 SHA-256
4. **先失效**：`memset(slot, 0)` 后写回 bootctrl —— 中断的 OTA 留下的是
   **不可启动的槽**，而不是一个摘要对不上的旧槽
5. 写载荷（非块对齐的尾部补零块）
6. **从介质回读**并重算 SHA-256 比对（不是从下载缓冲算）
7. 写 `image_size`、`image_version = 旧+1`、`sha256`

**`priority` 保持 0。** 激活是**单独的、也要校验的**操作。
（板上的 `nbootctl stage` 不同：它写完回读通过后**直接激活**，见
[ota.md](ota.md)。）

成功响应：`OKAYverified; activate separately`

`activate` 会：重新按 `image_size` 哈希介质内容并与 `sha256` 比对，
然后设 `active_slot`、`priority=15`、`tries_remaining=0`，
并把**同域另一槽**的 priority 从 15 降到 14。

## 写 AMP 槽

两条路径都走校验写入（当前 N-Boot）：

```sh
# 一条命令带文件名
fastboot flash amp_b amp.itb

# 或先 stage 再走 OEM
fastboot stage amp.itb
fastboot oem board:flash:amp_b
```

两者最终都到 `fastboot_oem_board("flash:amp_b")`。

**旧版 N-Boot（PR #10 之前）**：`fastboot flash amp_a|amp_b` 落到通用裸写
路径，**不更新元数据**。那时只能用 `oem board:flash:`。

### 实测：写入到底改什么

```
$ fastboot flash amp_a amp-shmem.itb
Writing 'amp_a'    OKAY

$ nbootctl status
amp_a priority=0 successful=0 size=36118016 version=2
amp_b priority=0 successful=0 size=0        version=0
```

写前 `amp_a` 是 `size=0 version=0`（分区从未被填过）；写后拿到了真实
`size` 与递增的 `version`。**裸写路径不会写这两个字段。**

⚠️ **`priority` 归 0 是设计行为。** `flash` 只写载荷，激活是独立操作。
但要注意它的副作用：如果槽此前是激活态（`priority=15`）而载荷为空，
写入后 `priority` 归 0 —— **这恰好解掉一类启动循环**：

> 症状：板子每次启动都先试 AMP、失败、回落到 NuttX。
> 原因：`active_slot` 指向的槽 `priority=15` 但 `image_size=0`，
> 校验必然失败。
> 处理：给它写一个真载荷，或把 `priority` 清 0。

### AMP 槽的启动与确认

AMP 域**只启动 `active_slot`**，不会自动换槽。

能否启动**只看 `priority != 0` 且 `image_size != 0`**，与 NuttX 域一致
（读码确认，`nboot_amp_load_slot()`）。`tries_remaining` **不参与判决**：
刚激活的槽按构造就是 `tries_remaining=0`，若要求非零，每个新写的槽都起不来
（这正是 [known-issues.md](known-issues.md) #3 里改掉的旧规则）。
现在的行为是：未确认且计数非零时，在**读载荷或启动任何内核之前**先持久化一次
递减；计数为 0 时不递减（无符号数，减了会回绕成 255）也不改写 bootctrl。
真正挡住坏镜像的是随后的 FIT 校验。

确认是**操作者动作**，不是自动应答：

1. 启动 AMP，观察 `nyampctl health` / `info` 成功
2. 重启回普通 NuttX
3. `nbootctl mark-successful amp a|b`

确认过的镜像之后不再消耗 trial。

### ⚠️ AMP 域是静默的 —— 别把"无输出"当失败

AMP FIT 的 bootargs 含 **`console=ttynull`**：Linux 与 openvela 都**不往
这个 UART 输出**。所以启动 AMP 之后串口一片空白是**正常的**。

判活要看 **openvela 侧**，而不是串口日志：

```sh
nyampctl info
```

实测成功的样子：

```
nyamp Linux info: generation=447580846
online=4
cpu0 part=0xd08
cpu1 part=0xd08
cpu2 part=0xd08
cpu3 part=0xd08
```

判据：

| 字段 | 期望 | 含义 |
|---|---|---|
| `online` | `4` | 四个 A53 全部起来了 |
| `part` | `0xd08` | Cortex-A53 的 part number |
| `generation` | 非零 | RPMsg 与 Linux 侧握手成功 |

**`generation` 非零是双域都活的硬证据** —— 它来自 Linux 侧写入、
openvela 侧读出，跨核通信没通就全是 0。

反过来说：**串口无输出 + 板子不回 N-Boot 重启循环** = AMP 正在跑。

## 写 N-Boot 自身

```sh
fastboot stage nboot-4mib.img
fastboot flash nboot
```

前置校验（全部通过才动介质）：6 个子镜像名精确为
`atf-1`、`uboot`、`fdt`、`atf-2`、`atf-3`、`optee`；每个都有 sha256 且无
`ignore`；`loadables` 恰好是 `{uboot, atf-2, atf-3, optee}` 各一次；
`uboot` 载入地址 `0x40200000` 且内嵌 FDT 的 `compatible` 含 `kickpi,k7`。

写 4 MiB 后**整块回读 `memcmp`**。

⚠️ **不宣称断电原子性**：供应商 SPL 按 2 MiB 间距探测候选，
而可互操作的 FIT 布局占 4 MiB，两者重叠，只能单区域原位更新。

## N-Boot 镜像从哪来

**不要手搓。** N-Boot 仓只发布 proper（`.bin`）+ 控制 DTB 两件，
4 MiB FIT 由队伍仓的官方脚本组装：

```sh
cd contest2026_062_PharosTech
DATA_IMG= AMP_ITB= bash tools/k7_pack/build_nboot_ab.sh \
    <nuttx.bin> <nboot-release-dir> <rkbin-dir> <out-dir> emmc
```

它从 rkbin 的 `rk3576_bl31_v1.24.elf` 用 `dd` 切出 atf-1/2/3，
用 rkbin 的 `mkimage -E -p 0x1000` 打包，断言 6 个 payload 512 字节对齐、
FIT ≤ 4 MiB，再 `truncate -s 4194304` 补齐。

rkbin 版本是**钉死的**（`ecb4fcbe...`，不跟 master），用
`tools/k7_pack/fetch_rkbin.sh` 取。

## 常见错误

| 报错 | 原因 |
|---|---|
| `FAIL invalid recovery partition layout` | 目标盘没有合法布局，见 [emmc-provisioning.md](emmc-provisioning.md) |
| `FAIL partition is not in recovery allowlist` | 槽名不在 `nuttx_a/nuttx_b/amp_a/amp_b` 里 |
| `FAIL recovery rejected (-8)` | `-ENOEXEC`，载荷形状与域不符 |
| `FAIL recovery rejected (-10)` | `-ECHILD` 类，通常尺寸超分区容量 |
| `cannot load` | 文件超过 64 MiB 下载缓冲，或路径含非 ASCII |
| 写入成功但启动的是旧固件 | 没冷启动，或另一块介质抢先 |
