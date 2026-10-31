# `bootamp` 内存启动：不写盘的 AMP 迭代回路

`bootamp <addr> <size>` 从 RAM 里的 FIT 直接冷启 AMP 双域，**不碰任何槽、
不改 bootctrl、不消耗 trial**。失败的代价是一次整机复位，回到原来的 NuttX 槽。
做 AMP 镜像（Linux 内核、DTB、initramfs、openvela 固件任一变动）时，
先走这条回路，稳定后再考虑写 `amp_a|b`。

置信度标注：**实测确认** / **读码确认**（读 N-Boot 源码 `cmd/bootamp.c`、
`lib/nboot_amp.c`、`include/nboot_amp.h` 得出，未逐条上板对照）/ **仅推断**。

## 回路

```
nsh> reboot                           （或 nbootctl reboot fastboot）
  串口持续发 !  → N-Boot>
N-Boot> fastboot usb 0
PC$    fastboot stage amp.itb         仅下载到 RAM 0x60000000，不写盘
  串口发单个 ETX (0x03) → 退出 fastboot，回到 N-Boot>
N-Boot> bootamp 60000000 <hex-size> check
        bootamp: FIT preflight passed; no CPU or payload changed
N-Boot> bootamp 60000000 <hex-size>
        bootamp: launching Linux on 0x100; NuttX waits on CPU0
```

`<hex-size>` 是 FIT 文件字节数的十六进制，不带 `0x`
（`printf '%x\n' $(stat -c %s amp.itb)`）。实测用过的例子：
`bootamp 60000000 cf5800`（13588480 B）、`bootamp 60000000 21a9200`。

置信：实测确认（2026-09-10、09-14、09-20 多轮）。

### 串口的两个硬约束（实测确认，2026-09-10）

- **行尾只发单个 CR。** 串口桥发 CRLF 时，N-Boot 把 LF 当成一条空命令并
  **重复执行上一条命令**；如果此刻在 fastboot 里，这个多余字节还会**立即把
  fastboot 踢出去**（任意键即退出）。脚本里逐条发 `cmd + "\r"`。
- **退出 fastboot 发单个 ETX（0x03）。** fastboot 收到任意键就退出
  （见 [commands.md](commands.md)「退出」）；历次实测用的都是单个 ETX。

### `check` 先行

`check` 只做校验：不加载载荷、不启动 CPU、不改任何状态，失败也不复位。
真跑一旦越过 "launching Linux" 就**没有软失败**——任何后续问题都是
`reset_cpu()`（六个复位原因见 [known-issues.md](known-issues.md) #8）。
所以顺序固定：`check` 通过再真跑。

| 输出 | 含义 |
|---|---|
| `bootamp: FIT preflight passed; no CPU or payload changed` | 合法 |
| `bootamp: FIT rejected before loading (-22)` | `-EINVAL`：结构/地址/DTB 契约不符 |
| `bootamp: FIT rejected before loading (-129)` | `-EKEYREJECTED`：缺 sha256、带 `ignore`、或哈希不符 |
| `bootamp: cold CPU topology required (-16)` | `-EBUSY`：其余 7 个核（1/2/3、0x100..0x103）有不在 OFF 态的；`-22` 则是当前不在 CPU0 上。只在真跑时出现。历史上曾因 PSCI 驱动未 probe 而误报 -16（2026-09-10，已修） |
| `bootamp: unset fdt_high and initrd_high for bounded relocation` | 环境里设了这两个变量 |
| 直接返回失败、无输出 | 地址/长度不合法：`size > 0x20000000`、区间不在 DRAM、与 N-Boot 自身驻留区重叠、或与 `[0x42000000, 0x60000000)` 装载窗口重叠 |

所以 stage 地址用 `0x60000000`（fastboot 下载缓冲，恰在装载窗口之上）。
缓冲 64 MiB，超过的 FIT `fastboot stage` 直接 `cannot load`。

### 失败后读 Linux 日志（实测确认，2026-09-10）

AMP 的 bootargs 是 N-Boot 写死的
`rdinit=/init console=ttynull clk_ignore_unused pd_ignore_unused cpuidle.off=1`，
Linux 没有串口输出。热复位不清 DRAM：复位回 N-Boot 后用内核 ELF 的
`__log_buf` 符号换算物理地址（无 KASLR 时 `_text` 对应 `0x42000000`），
`md.b <phys> 10000` 直接读回内核日志。不必为此做一份带早期串口的内核。

## N-Boot 校验的 FIT 契约（读码确认）

FIT 外层规则（4 个子镜像、`/configurations/conf`、哈希、对齐）见
[images.md](images.md) 的「AMP FIT 契约」。这里补 openvela 子镜像与 DTB 的
**精确判据**，即做产品形态时真正会撞上的那些。

### `openvela` 子镜像

| 判据 | 值 |
|---|---|
| `load` 与 `entry` | 都必须是 `0x4a400000` |
| `cpu` | `0` |
| `type` / `arch` / `compression` | `firmware` / `arm64` / `none` |
| 文件长度 | ≤ 16 MiB，且 ≥ 64 字节 |
| 偏移 56 | `ARMd` |
| Image 头偏移 16 的 `runtime_size`（LE u64） | **≥ 文件长度 且 ≤ 16 MiB** |
| Image 头偏移 24 的 flags bit0 | 0（小端） |

**限制的是 `runtime_size`，不是文件大小。** 带大 BSS/NOLOAD 段的镜像文件可以
很小而 `runtime_size` 超标。product 镜像实测 `image_size=0x613000`，放得进。

`linux` 子镜像同理（上限 80 MiB），另要求 `load - text_offset` 按 2 MiB 对齐、
`cpu=0x100`。

### DTB

| 项 | 规则 |
|---|---|
| `/cpus` | 每个 `device_type="cpu"` 节点都必须是 `arm,cortex-a72` + `enable-method="psci"`，affinity ∈ `0x100..0x103`，四个齐全不重复；`status` 若存在须为 `okay`。**出现任何 A53 cpu 节点即拒。** 名字叫 `cpu`/`cpu@*` 却没有 `device_type="cpu"` 也拒 |
| `/reserved-memory` | `#address-cells=2`、`#size-cells=2`、`ranges` 存在且为空 |
| `/serial@2ad40000` | `status="disabled"`（UART0 归 openvela） |
| `rockchip,fiq-debugger` | 所有此 compatible 的节点 `status="disabled"` |
| `rockchip,amp` | 恰好一个，`status="okay"` |
| 四块固定预留 | 路径、基址、大小精确匹配，各带 `no-map`，`reg` 高 32 位为 0 |

| 路径 | 基址 | 大小 |
|---|---|---|
| `/reserved-memory/rpmsg@47800000` | `0x47800000` | `0x200000` |
| `/reserved-memory/rpmsg-dma@47a00000` | `0x47a00000` | `0x200000` |
| `/reserved-memory/amp-shmem@47c00000` | `0x47c00000` | `0x400000` |
| `/reserved-memory/openvela@4a400000` | `0x4a400000` | `0x1000000` |

**额外的 reserved-memory 节点不被检查，也不被拒绝。** 校验只按路径查这四个，
不遍历其余子节点。这是产品形态能加自己的堆区而不改 N-Boot 的原因。

### `amp-irqs`

- 每条路由 6 个 u32；总长是 6 的整数倍，**至少 2 条、至多 16 条**。
- 每条：`[0]=0`、`[1]=SPI 中断号`、`[2]=0`、`[3]=0x80`（优先级）、`[4]=0`、`[5]=0`。
- **必须包含 108（UART0）与 174（邮箱）。** 缺任一即拒。
- 其余路由**不限中断号**，只要格式对就接受。

上限 16 是为了让校验器不去走一张无界的表，不是硬件限制。

⚠️ 运行期握手（30 s 超时）在 GIC 侧只检查 108 与 174 两条：target 等于 CPU0 的
掩码、priority 等于 `0x80`（另外还等 GIC 就绪标记、邮箱 LINK 消息与 vring 接收
缓冲就位）。其余中断 N-Boot 不管——谁路由、什么时候路由，是两个 OS 之间的事。
见下文 GIC 一节。

## 把完整 product 固件塞进这份契约（N-Boot 零改动）

**实测确认**（2026-09-20）：带 WiFi/双屏/eMMC/音频/USB/眼睛/Web 的 product 固件
与 Linux 4×A72 并存，全程 `fastboot stage` + `bootamp 60000000 <size>`，
没写任何槽。此前 AMP 下只跑过 16 MiB 的最小 openvela（nsh + nyampctl）。

三处改动都在契约**允许的空隙**里：

### 1. 第二堆区挪到别处

16 MiB 只够放镜像和主堆。大堆放进 Linux DTB 另行预留的区域：

- `RAM_START=0x4a400000`、`RAM_SIZE`=16 MiB
- 新增 `RK3576_RAMBANK2_ADDR=0x70000000`、`RK3576_RAMBANK2_SIZE_MB=256`
- `ARM64_GICV2_PREINITIALIZED`、`ARM64_GICV2_STATIC_SPI`
- `DEV_SIMPLE_ADDRENV`、`RK3576_RPTUN`、`RPMSG_CHAR`、`OPENAMP_CACHE`

`RK3576_RAMBANK2_ADDR` 默认 0 = "OP-TEE 之后到 DDR 末尾全是我的"，那是 openvela
独占整板时的语义；AMP 下那片内存大半属于 Linux，必须显式指到 DTB 预留的那块。

### 2. GIC 自路由

Linux 先初始化 distributor，并把它不认识的 SPI 都路由给自己。做法：NuttX 在
`STATIC_SPI` 模式的 `up_enable_irq()` 里，对 `irq >= GIC_IRQ_SPI` **按字节**写
`IPRIORITYR = 0x80`、`ITARGETSR = 1 << 本核`——"本 OS 挂了 handler 的 SPI 就是
它拥有的"，自己拉过来（`tools/amp/nuttx/gicv2-amp-selfroute.patch`）。
这样 product 要用的中断不必都挤进 ≤16 条的 `amp-irqs`（这条动机**仅推断**，
补丁注释只说明了机制）。

优先级与目标寄存器是字节可寻址的：只写自己那一个字节，不会与对端更新同一个
32 位字里相邻中断的操作竞争。不要改成 32 位读-改-写。

### 3. Linux DTB 交出所有权

`tools/amp/linux/amp_product_dtb.py BASE.dtb OUT.dtb --cru-header rockchip,rk3576-cru.h`
（在编译好的 dtb 上做 dtb→dts→改→dtb，只需要 `dtc` 和内核的时钟 ID 头）：

| 类别 | 改动 | 为什么 |
|---|---|---|
| 内存 | 加两块 `no-map`：`openvela-dma@49400000`（16 MiB）、`openvela-heap@70000000`（256 MiB） | 给 openvela 的 DMA 堆与第二堆区 |
| 节点 | 禁用 `mmc@2a330000`(eMMC)、`mmc@2a310000`(SD)、`sai@2a610000`(SAI1)、`tsadc@2ae70000`、`watchdog@2ace0000`、`adc@2ae00000`(SARADC) | 见下「坑三」：`okay` 的节点即使没编驱动也有副作用 |
| 电源域 | PD 5/6/7/10（NVM、SDGMAC、USB、AUDIO）标 `rockchip,always-on` | 无 Linux 消费者的域会被关掉 |
| 时钟 | `rockchip-amp` 节点持有 `PCLK_MAILBOX0`、eMMC 五个时钟、`ACLK/HCLK_NVM_ROOT`、`CLK_PWM2`/`PCLK_PWM2` | `clk_ignore_unused` 管不到厂商内核自己 28 s 后的 "unprotect" 工作，它会关掉 PWM2（LCD 背光） |

### 三个失败签名

| # | 现象 | 根因 | 怎么认 |
|---|---|---|---|
| 一 | 刚进 C 运行时就异常 | 第二堆区挪走后，`0x4a400000` 这 16 MiB **没人做 MMU 映射**（原先被"bank2 从 OP-TEE 之后一路到底"顺带盖住）。修法：`RAMBANK2_ADDR != 0` 时补一条 `DRAM_KERNEL` 映射，覆盖 `RAM_START..RAM_SIZE` | 早，且与外设无关 |
| 二 | `irq_attach(220)` 处打印 `"Synchronous Abort" handler, esr 0x96000035`，随后 `Resetting CPU` | PD_AUDIO 被 Linux 关掉，碰 SAI1 寄存器是总线错误。**打印来自 N-Boot 残留在 EL2 的异常向量**——AMP 下外部中止落到 EL2 | 看到 **U-Boot 风格的异常打印 + 整机复位**，别判成 N-Boot 自己崩了；去查刚访问的外设的电源域/时钟归谁 |
| 三 | eMMC 挂载成功**约 9 s 后** `emmc data irq cmd=00000451 ... eint=0011`，之后永远 timeout | Linux DTB 里 `mmc@2a330000` 仍是 `okay`。**没编 MMC 驱动也一样**：OF 核心照样应用 `assigned-clocks`（200 MHz / 24 MHz），在 openvela 背后把卡时钟改了 | 延迟固定、与负载无关。已排除：把所有稳压器标 always-on 无效 |

三条都是**实测确认**（2026-09-20）。共同教训：AMP 下"对端没驱动"不等于
"对端不碰"。DTB 节点、电源域、时钟三层都要显式交出。

### 实测结果（2026-09-20）

AMP 下 product 固件启动到 NSH；`audio ready: /dev/audio/pcm0 and pcm_in0`；
`/data`、`/config` 挂载；SV6621 WIFIREADY/BTREADY 并 DHCP 联网；双 LCD +
nyabula_eye；`nyampctl health ok capabilities=0x3`、`nyampctl info online=4`；
ADB 正常；经 WiFi 取 976858 B 面板资源 200（1.45 s）；4×20 并发请求全部应答；
连续 3.5 分钟 eMMC 零错误。从 AMP 域执行 `reboot` 可干净回到普通 product 固件。

### 未做 / 已知缺口

- **没写过 amp 槽。** 以上全是 RAM 启动。槽位启动路径见 [images.md](images.md)。
- `tools/amp/validate_amp_layout.py` 还不认 product 形态（UP、`MM_REGIONS=2`、
  `DMA_ALLOC`），组 FIT 时目前绕过它。`build_amp_fit.sh` 的检查是按最小形态
  （四核 SMP、私有堆范围）写的。
- LLM 服务并入、模型从 eMMC 交给 Linux、Agent 桥接均未做。

### 构建机上的两个坑（实测确认）

- OpenAMP 第三方源码不全：`open-amp.zip` 在但目录缺文件，`.depend` 报
  `没有规则可制作目标 open-amp/lib/utils/utilities.c`。从同版本兄弟工作区拷入
  已打补丁的 `open-amp`/`libmetal`，再 `make -C openamp context TOPDIR=$PWD`
  重新生成 `include/openamp`——否则 `virtio_alloc_buf` 参数个数对不上，那是
  陈旧头文件，不是代码错。
- `pip install kconfiglib` 后 `make olddefconfig` 可用，能自动补齐新符号默认值，
  不用手工 `kconfig-tweak` 逐个设。
