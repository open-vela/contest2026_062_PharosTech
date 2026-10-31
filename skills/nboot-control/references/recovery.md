# 恢复与排错

## 先判断板子在哪一层

| 串口显示 | 层 | 能做什么 |
|---|---|---|
| `N-Boot>` | U-Boot 控制台 | 全部命令，fastboot 的入口 |
| `fastboot` 且 `fastboot devices` 有输出 | Fastboot | 只能下载与写盘 |
| `nsh>` | NuttX 已启动 | 只能 `nbootctl` |
| 完全无输出 | 挂在某处或 AMP 域 | 见下 |

**AMP 域启动后串口是静默的。** AMP FIT 的 bootargs 里
`console=ttynull`，这个域没有控制台输出。所以"板子静默"不代表挂了，
可能就是进了 AMP 域。

### 区分「AMP 在跑」与「板子挂了」

两个判据，**前者比后者可靠**：

| 观察 | 结论 |
|---|---|
| 串口无输出，且 **10 秒以上没有重复的启动日志** | AMP 在跑（正常） |
| 串口无输出，但**反复出现同一段 U-Boot 启动日志** | bootamp 失败后复位循环 |
| 串口无输出，且**完全不响应任何按键** | 真挂了，需断电 |

"没有重启循环"是关键 —— `bootamp` 失败会 `reset_cpu()` 整个 SoC，
所以卡在 AMP 里和挂在 AMP 里的区别就是**有没有反复启动**。

**要确认 AMP 真的活着**，不能用串口，要用 `nyampctl`：

```
# 先让它回到 openvela（AMP 里的 A53 域）能接受输入的状态
# 然后：
nyampctl info
```

看到 `online=4` 和 `generation` 非零就是活的。详见
[images.md](images.md) 的「AMP 域是静默的」。

⚠️ **AMP 域的 `uname` 与主域不同**。如果串口有 nsh 提示符，
`uname -a` 能区分你在哪个域：

| 固件 | `uname` 时间戳 |
|---|---|
| 主域 product 固件 | `Sep 18 2026 20:02:16` |
| AMP 域 openvela | `Sep 18 2026 01:56:25` |

时间戳不同 = 两套不同的 openvela 构建，这正是 AMP 双域的形态。

## NuttX 起来了但卡死（nsh / adb / HTTP 都不回）

N-Boot 不是看门狗，系统起来之后挂了它不管，也不会换槽。此时 `adb reboot`
通常也无效。固件若带了 `TTY_FORCE_PANIC` + `BOARD_RESET_ON_ASSERT=2`，
从串口连发 3 个 `0x1F`：先得到全部线程的 dump（含 CPU 占用），然后自动复位。
配置、输出原文与边界见 [known-issues.md](known-issues.md) #13。

判断前先排除一个假象：`adb shell` 的输出尾部会丢，空输出不等于没反应
（[known-issues.md](known-issues.md) #12）。用串口复核。

## 重启后停在 `N-Boot>`，报 `MMC device 1 is unavailable`

热重启后 eMMC 首次初始化失败，`bootcmd` 顺次落到 fastboot，又被串口上的输入
中止（`Operation aborted.`）。**先 `mmc dev 1` 再试一次**（实测第二次成功），
然后 `bootnuttx`。若进了 NuttX 却没有 `/dev/mmcsd1`（`reboot` 与强制 panic 复位
都试过无效），下一步是断电冷启动——**断电是否恢复在日志里尚未回填，仅推断**。
详情与未解决的部分见 [known-issues.md](known-issues.md) #11。

## 三个恢复入口

按可靠度排序：

### 1. 串口 `!`（首选）

单进程内先发 `reboot\r` 再**持续发 `!`**，跨复位边界保持发送。

```sh
python3 tools/nboot/request_recovery.py --port COM11
```

关键点：

- **必须单进程独占串口。** 分两个进程会互抢端口，`!` 全被其中一方吃掉，
  症状是板子重启了但进不去控制台。
- 发送方要在**复位之前**就开始，一直发到看到
  `serial recovery requested`。
- 板子自己的轮询窗口只有 **100 ms**（10 次 × 10 ms），所以发送间隔要
  远小于这个（脚本用 10 ms）。

参考实现（`!` 跨复位）：

```python
with serial.Serial(port, 1500000, timeout=0) as d:
    d.write(b"reboot\r")
    deadline = time.monotonic() + 25.0
    while time.monotonic() < deadline:
        d.write(b"!")
        d.flush()
        observed.extend(d.read(4096))
        if b"serial recovery requested" in observed:
            break
```

### 2. 硬件恢复键

按住恢复键上电。SARADC 通道 1，读数 `< 100` 判为按下，
直接进 Fastboot（不是控制台）。

### 3. 断电冷启动

最彻底。会重新走 BootROM → SPL → N-Boot。

⚠️ **冷启动后板子会按介质选择策略重新选盘**，想让它从 eMMC 起就得
拔掉 SD 卡。

## 静默板子的判断流程

```
有输出？
├─ 有 → 看提示符，按上表处理
└─ 无
   ├─ 断电重启 → 有没有启动日志？
   │  ├─ 有 → 是运行态挂住，用 `!` 进入
   │  └─ 无 → 引导链坏了，见"引导链损坏"
   └─ 是否从 SD 启动过 AMP？ → 可能只是 AMP 域静默
```

## 引导链损坏

症状：断电重启后 BootROM 阶段都不出字（连
`U-Boot 2026.10-rc2 ...` 都没有）。

原因通常是：`uboot` 分区（LBA 16384）被写坏，或 `MiniLoader`（LBA 64）
被写坏。

**恢复**：用另一块介质上的完好系统启动，然后从板内写回。

板内克隆（从 SD 读、写 eMMC）：

```
mmc dev 0; mmc read  0x42000000 0x40 0x2c0        # 读 SD 的 loader（360448 B = 704 扇区）
mmc dev 1; mmc write 0x42000000 0x40 0x2c0        # 写 eMMC 的 LBA 64
```

写完后 `crc32 0x42000000 0x58000` 与源比对。

## 板子频繁自重启

**症状**：串口不断重复同一段启动日志。

**最常见原因**：AMP 槽里有个启动不起来的 FIT。启动链是
`bootamp → bootnuttx → fastboot`，`bootamp` 失败后会回落，但如果 AMP
槽被反复激活（`active_slot` 指向它、`priority != 0`），每次启动都会先试它。

**处理**：进控制台后清掉那个槽：

```
# 方式一：从 NuttX 侧
nbootctl set-active amp b        # 切到另一个槽（如果它是好的）

# 方式二：直接覆盖 bootctrl（会重置全部状态）
```
然后从主机写一份干净的 bootctrl.img 到 LBA 32768。

**注意**：覆盖 `bootctrl.img` 会把 NuttX 槽状态也重置。用
`tools/nboot/bootctrl.py init` 生成的默认态是 A 槽 priority=15、
B 槽 priority=14、AMP 域全 0。

## USB 相关

### `fastboot devices` 没输出

- 板子不在 fastboot 模式（最常见）
- USB 线插错口：K7 上只有 **OTG 口**（DWC3 @ `0x23000000`）能当设备
- 主机驱动问题

### 写入成功但主机报"未知 USB 设备"

**历史缺陷**：N-Boot 退出 fastboot 时的清理顺序。

退出序列原本是：

```c
udc_device_put(udc);      /* device_remove → dwc3_remove：关 PHY、解映射寄存器 */
g_dnl_unregister();       /* 才轮到 usb_gadget_disconnect() */
```

`udc_device_put()` 会 `device_remove()` 掉 DWC3，而 DWC3 的 remove 会
注销 PHY、`kfree` 控制器结构、并 `unmap_physmem()` 掉寄存器映射。
之后 `g_dnl_unregister()` 才走到 `usb_gadget_disconnect()` →
`dwc3_gadget_pullup(g, 0)`，打在**已释放的对象和已解映射的寄存器**上。

**修法**：交换两行，先 unregister 再 put。`g_dnl_unregister()` →
`usb_composite_unregister()` → `usb_gadget_unregister_driver()` →
`usb_gadget_remove_driver()`，那里**有** gadget 指针，且
`dwc3_gadget_stop()` 只碰自己的寄存器，控制器还映射着，安全。

```c
exit:
	g_dnl_unregister();
	udc_device_put(udc);
	g_dnl_clear_detach();
```

顺带修了 `g_dnl_register()` 失败时**直接 return** 导致 UDC 泄漏的问题
（应 `goto exit`）。

**验证方式**：进 fastboot → `fastboot devices` 有输出（`18d1:d00d`）→
退出 → 检查主机 `Get-PnpDevice` 里该设备**消失**（而不是变成
`VID_0000` 的未知设备）。

⚠️ **注意区分**：主机上可能存在与板子无关的 `VID_0000&PID_0002`
未知设备（例如与 CH340 共用父集线器的另一路）。用 `InstanceId` 的
父设备段区分，不要误判成板子。板子的 fastboot 设备 `InstanceId` 含
板子的 `serial#`。

### 上位机看不到 rockusb 设备

**已知且无解（除非改 gadget VID）**：N-Boot 用 `0x18d1:0xd00d`，
Rockchip 工具按 `0x2207` 过滤。`upgrade_tool LD` 会回
`List of rockusb connected(0)`。

绕过：用 `fastboot stage` + 板内 `mmc write`，见
[emmc-provisioning.md](emmc-provisioning.md)。

## 介质选错

**最危险的错误**：SD 和 eMMC 用同一套 GPT 布局，写错不会报错。

判断当前介质：

```
fastboot getvar nboot-medium        # sd | emmc
```

显式指定：

```
fastboot oem board:target:emmc      # 本次会话
```

⚠️ `target` 在**拔线后复位**为 `auto`，不是持久的。分多次调用时要注意。

板内判断：

```
mmc list                            # 看两个设备
mmc dev 0; part list mmc 0          # SD 的布局
mmc dev 1; part list mmc 1          # eMMC 的布局
```

## 只有冷启动才生效的操作

| 操作 | 为什么 |
|---|---|
| 新写的 N-Boot | BootROM 只在冷启动时加载 SPL 与 N-Boot |
| 新的 `trust.img` | 同上 |
| 分区表变更 | N-Boot 启动时缓存介质选择结果 |

`version` 命令显示的是**内存中正在运行的**镜像版本，不是盘上的。
判断盘上的版本要读回 LBA 16384 算 CRC32 与主机文件比对。

## 必须冷启动 + 移除另一块介质

要让 eMMC 上的系统真正生效：

1. 拔掉 SD 卡
2. 断电重启

SD 卡在槽里且布局有效 → `nboot_storage_boot_devnum()` 永远先返回 SD。
这是**设计行为**（允许用恢复卡修 eMMC），不是 bug。
