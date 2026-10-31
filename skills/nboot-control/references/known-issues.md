# 已知缺陷与规避

按发现顺序。每条都注明「谁引入的」「怎么绕」「验证状态」。

---

## 1. fastboot 退出顺序拆坏 DWC3（已修）

**谁引入**：上游 U-Boot 原有顺序，N-Boot 继承。上游多数平台
`device_remove()` 是空实现（`CONFIG_DM_DEVICE_REMOVE=n`），所以不会暴露；
K7 开了这个配置才触发。

**症状**：每次 `fastboot flash` / `stage` 成功并退出 fastboot 之后，
主机在**该端口**上记录一个
「未知 USB 设备(设备描述符请求失败)」，`VID_0000:PID_0002`。

**根因**：`cmd/fastboot.c` 的退出序列

```c
exit:
	udc_device_put(udc);      /* device_remove → dwc3_remove */
	g_dnl_unregister();       /* 才轮到 usb_gadget_disconnect() */
```

`udc_device_put()` → `device_remove()` → `dwc3_generic_remove()`
（`drivers/usb/dwc3/dwc3-generic.c`）：

```c
dwc3_remove(dwc3);                     /* core_stop + core_exit + kfree */
dwc3_shutdown_phy(dev, &priv->phys);   /* PHY 断电 */
unmap_physmem(dwc3->regs, MAP_NOCACHE);/* 寄存器映射解除 */
```

之后 `g_dnl_unregister()` 才走到 `usb_gadget_disconnect()` →
`dwc3_gadget_pullup(g, 0)` → `dwc3_gadget_run_stop()`。
**D+ pullup 从未被有序拉低** —— 它是随控制器一起消失的。

**修法**：交换两行。

```c
exit:
	g_dnl_unregister();
	udc_device_put(udc);
	g_dnl_clear_detach();
```

`usb_gadget_remove_driver()` 里**有** gadget 指针，且
`dwc3_gadget_stop()` 只碰自己的寄存器（不 `kfree`、不解映射），
所以此刻执行是安全的。**断开变成一次正常的 pullup 释放。**

**顺带修**：`g_dnl_register()` 失败时原本直接 `return`，
`udc_device_put(udc)` 被跳过 → UDC 泄漏，下次 fastboot 复用污染状态。
改为 `goto exit`。

### ⚠️ 这个修复**不能**消除那条「未知 USB 设备」

**幽灵设备依然存在**，原因不同：

pullup 拉低之后紧接着 `dwc3_shutdown_phy()` 把 USB2 PHY 断电，
端口上短暂呈现一个不应答的设备，Windows 把失败的节点缓存下来。

**它是暂态的**：一旦有真正的 gadget 重新枚举（比如 `bootnuttx` 之后
NuttX 的 ADB gadget），该条目被真实设备替换并消失。

实测对照：

| 时刻 | 幽灵设备 | 真实设备 |
|---|---|---|
| 停在 N-Boot（刚退 fastboot） | 存在 | 无 |
| `bootnuttx 1` 之后 | **消失** | `Android ADB Interface` OK |

**判断要点**：看到「未知 USB 设备」时，**先看板子在哪一层**。
如果它停在 N-Boot 而没启动 NuttX，那条记录是预期的，不是故障。
启动 NuttX 后仍在，才是问题。

**不要被无关设备误导**：主机上可能有别的
`VID_0000&PID_0002`（例如与 CH340 共用父集线器的另一路）。
用 `InstanceId` 里的父设备段和序列号区分——板子的设备带板子自己的
`serial#`。

---

## 2. `fastboot flash amp_a|amp_b` 绕过了校验路径（已修）

**谁引入**：N-Boot 自己的 fastboot 拦截列表（`drivers/fastboot/fb_command.c`）。

**症状**：写了 AMP 槽，但 bootctrl 没更新，启动器看不到它。
`active_slot` 指向一个从未被填过的槽，重新启动时该域无可启动槽。

**根因**：拦截只覆盖 `nuttx_a` / `nuttx_b` / `nboot`：

```c
if (!strcmp(cmd_parameter, "nuttx_a") || !strcmp(cmd_parameter, "nuttx_b")) {
	fastboot_oem_board(...);
	return FASTBOOT_COMMAND_FLASH;
}
if (!strcmp(cmd_parameter, "nboot")) { ... }
else { flash(cmd_parameter, response); }   /* ← amp_a / amp_b 落到这里 */
```

**修法**：四个恢复槽名合成一张表，任一命中都走 `fastboot_oem_board()`。
名字仍要过该函数自己的白名单，所以只是扩大了**走校验路径的拼写**，
没有扩大可写的分区集合。

**踩坑**：第一版用 `snprintf(parameter, ..., "flash:%s", slots[slot])`
运行时拼字符串，**消灭了 CI 用来验证"校验路径确实编进固件"的契约串**
`flash:nuttx_a`。full profile 的构建 job 因此失败。

CI 的 `Verify binary contract` 步骤 greps 二进制里的字面量：

```sh
strings build/u-boot-nodtb.bin | grep -q 'flash:nuttx_a'
```

改回**静态字面量表**（`{ "nuttx_a", "flash:nuttx_a" }, ...`）即可。
教训：动这个文件前先看 CI 断言了哪些字符串。

**验证**：板上实测 ——
`fastboot flash amp_a amp-shmem.itb` → `Writing 'amp_a' OKAY`，
之后 bootctrl 里 `amp_a size=36118016 version=2`
（旧路径只裸写，不写 size/version）。

**规避（旧版本 N-Boot）**：AMP 槽**必须**用
`fastboot oem board:flash:amp_b` + `fastboot oem board:activate:amp_b`。

---

## 3. `nboot_amp_load_slot()` 里一行被写坏（已修）

**谁引入**：`aefcfd1790e`（加 AMP 槽载荷校验的那个 commit）。

**症状**：`test_amp_slot.py` 在 `cc -Werror` 阶段就失败，**整个测试从未运行过**。

**根因**：`cmd/bootnuttx.c` 里一处复制粘贴事故：

```c
	ret = part_get_info_by_name(storage.desc, active ? "amp_b" : "amp_a",
				    &partition);(storage.desc, active ? "amp_b" : "amp_a",
				    &partition);
```

多出来的尾段是逗号表达式，运行时无副作用，但触发 `-Wunused-value`，
在 `-Werror` 下编译失败。

**修法**：删掉尾段。

**连带发现**：该测试的**断言也没跟上实现**——它仍按旧的"tries 用尽即拒绝"
规则断言，而实现已刻意改成"只看 priority"（正是为了让刚激活的槽能启动）。
两处断言按当前规则更新，并补上了原意要保护的行为：
消耗掉最后一次 trial 后槽仍可启动、且不再改写 bootctrl。

**验证**：七个主机测试全部通过（`test_amp_slot` 是此前一直红着的那个）。

---

## 4. `amp_a` / `amp_b` 不做布局校验（设计取舍，未改）

`nboot_layout[]`（`nboot_storage.c`）只列了
`uboot`/`trust`/`bootctrl`/`nuttx_a`/`nuttx_b`。AMP 两个分区**只按 GPT 名字
解析**，任何叫这名字的分区都会被接受，唯一边界是
`blocks <= partition.size`。

**含义**：AMP 槽的位置可以自由调整（只要改分区表），代价是没有防呆。
写 AMP 前先确认分区确实存在且够大。

---

## 5. `current-slot` getvar 是硬编码（未修）

`fb_getvar.c` 里 `current-slot` 直接返回 `"a"`，**与 bootctrl 无关**。

**规避**：不要用它判断槽位。真实槽位从 PMU1 GRF 交接头读
（`nbootctl status`），或看启动日志的
`bootnuttx: booting NuttX slot %c`。

---

## 6. `oem board:target:*` 只在本次 USB 会话有效

**症状**：脚本分多次调用 fastboot，第二次之后介质变回 `auto`。

**根因**：`nboot_recovery_reset()` → `nboot_storage_reset_target()`，
在每次 `fastboot` 命令启动时（`cmd/fastboot.c`）和 USB 断开时
（`fastboot_disable()`）都会被调用。

**规避**：每次需要指定介质时重新发 `oem board:target:emmc`，
或在一次调用里做完。另外 `selected_devnum` 在进程生命周期内是**粘的**——
所以 `target:auto` 会落到最先打开的那个介质，不是重新探测。

---

## 7. 其他 gadget 命令不受退出顺序影响

检查过 `cmd/ums.c`、`cmd/dfu.c`、`drivers/usb/gadget/ether.c`：
它们的退出路径**没有** `cmd/fastboot.c` 那种
`udc_device_put` + `g_dnl_unregister` 的组合，所以不存在同一个问题。

`drivers/usb/gadget/ether.c` 只调 `udc_device_put(dev->parent)`，
由 UDC 层的 `usb_gadget_remove_driver()` 统一处理 gadget 侧。

**将来加 gadget 命令时注意**：如果自己配对 `udc_device_put` 与
`g_dnl_unregister`，先 unregister（会走到 `usb_gadget_disconnect()`），
再 put（会 `device_remove` 掉控制器）。顺序反了就是第 1 条那个
use-after-unmap。

---

## 8. AMP 试跑失败即复位整个 SoC

**行为**：`bootamp` 在 Linux 启动之后的任何超时（GIC/RPMsg 就绪等待
30 秒超时、mailbox 契约不符、CPU_ON 失败等）都会打印原因然后
`reset_cpu()`。六个复位原因：

- `BL31 AMP argument service unavailable`
- `cannot arm fresh GIC handshake`
- `Linux CPU_ON failed`
- `Linux RPMsg buffer contract mismatch`
- `Linux GIC/RPMsg readiness timed out`
- `NuttX entry returned`

**含义**：没有"返回错误码"这种软失败。调试 AMP 时准备好串口抓完整日志，
否则只能看到重启循环。

**这不是看门狗**：已确认过的内核之后崩了，仍需手动恢复。

---

## 9. N-Boot 自更新不宣称断电原子性

供应商 SPL 按 2 MiB 间距探测候选，而可互操作的 FIT 布局占 4 MiB，
两者重叠。N-Boot 因此只做**单区域原位更新**：写入前完整校验 FIT，
写完整块回读 `memcmp`。

**含义**：写入过程中断电 → `uboot` 分区损坏 → 板子起不来，
需要从另一块介质恢复（见 [recovery.md](recovery.md)）。

---

## 10. N-Boot 仓的 PR 模板是上游原文

`.github/pull_request_template.md` 是 U-Boot 上游的内容，
声明"不要走 GitHub PR、走邮件列表"。与仓库自己的 `n-boot/main` PR
流程冲突。

**处理**：提 PR 前替换，或忽略模板内容。

---

## 11. 热重启后 eMMC 在 N-Boot 首次初始化失败（未解决）

**验证状态**：现象实测确认（2026-09-20，一次事故）；原因**仅推断**。

**症状**：NuttX 里 `adb shell reboot` 后，板子没进系统，停在 N-Boot 控制台。

```
bootnuttx 报：MMC device 1 is unavailable
→ bootcmd 顺次落到 fastboot usb 0
→ 被串口输入中止，打印 Operation aborted.
```

**当场处置**：手动 `mmc dev 1`，**第二次成功**，随后 `bootnuttx` 能引导
（当次是 slot b）。

**同一事故的后半段更糟**：此后 NuttX 每次启动都是

```
storage: emmc has no supported partition table
eMMC not mountable: -15
```

且 `/dev/mmcsd1` **不存在**——是卡识别失败，不是分区表坏。对照证据：N-Boot 下
`mmc read` LBA 0-3 正常，保护 MBR 与 `EFI PART` 头完好，`part list` 9 个分区
齐全。串口强制 panic 复位、再次 `reboot` **均不恢复**。

**影响**：`/config`、`/data` 没挂上 → 无 WiFi 凭据 → 板子回落成热点
（192.168.4.1），token 也读不到。**数据未见损坏迹象**，只是 NuttX 这侧初始化
不了卡。

**推断（未验证）**：eMMC 停在某个只有断电/硬复位才清的状态（例如复位那一刻
正有写入在途）；N-Boot 的重试路径能把它拉回来，NuttX 的初始化序列不能。
**"断电即恢复"本身在日志里也还没有回填**——按日志原文是"待断电重启后确认"。

**规避**：

- 看到 `MMC device 1 is unavailable`：先别判盘坏，`mmc dev 1` 再来一次。
- NuttX 下 `/dev/mmcsd1` 消失而 N-Boot 能读盘：断电冷启动（不是 `reboot`，
  不是强制 panic）。
- 事故发生在"改配置后立刻 `reboot`"。写完配置稍等再重启是顺着上面推断的
  预防措施，**仅推断**，未做对照。

**若断电确认有效，要补的两件事**（日志所列，均未做）：N-Boot `bootcmd` 对
`bootnuttx` 失败做重试；NuttX reboot 前同步并让 eMMC 回到空闲态（或用 RST_n
硬复位）。

---

## 12. `adb shell` 的输出尾部会丢

**验证状态**：实测确认（2026-09-20）。原因未查。

**症状**：`adb shell ls <dir>` 只回目录头；`...; echo alive` 的 `alive` 丢失。
同一命令走串口输出完整。

**含义**：**空输出不等于"没有"。** 用 `adb shell` 判断"文件在不在""命令跑没跑完"
会误判。

**规避**：判断文件用串口 `ls -l`，或直接 HTTP 取；要可靠返回值的命令走串口。
`adb push`/`pull`/`forward` 不受影响（实测 `adb push` 308182 B、
`adb forward` 取 927718 B 均 sha256 一致）。

相关的一条 `adb forward` 行为（实测确认，2026-09-19）：它不会把 PC 侧关闭传到
板端 TCP，板端要等 20 s 空闲超时才发现；所以"关掉连接后立刻发生的事"不能用它验。

---

## 13. 系统卡死的自救：串口强制 panic + assert 后自动复位

**验证状态**：实测确认（2026-09-20）。

这不是缺陷，是为"整机卡死只能断电"准备的常驻手段。与 N-Boot 的关系：
**N-Boot 不会因为系统挂死而回滚或复位**（见 #8 末尾与 [ota.md](ota.md)），
所以得由系统自己提供出路。

**配置**：

```
CONFIG_TTY_FORCE_PANIC=y
CONFIG_TTY_FORCE_PANIC_REPEAT_COUNT=3        # 字符默认 0x1F
CONFIG_BOARD_RESET_ON_ASSERT=2               # assert 后自动复位
CONFIG_SCHED_CPULOAD_SYSCLK=y                # dump 里带 CPU 占用
```

**触发**（PC 端）：

```python
serial.write(bytes([0x1f] * 3))
```

**结果**：

```
dump_assert_info: Assertion failed Force panic by user.: at file: serial/serial.c:2318
```

随后 `dump_tasks` 列出全部线程（状态、栈、CPU 占用），然后**自动复位**：
DDR 训练 → N-Boot → NSH → 重新联网，全程无人工介入。

**用途**：

- 取证：卡死时第一手的线程状态与 CPU 占用。基线参考：IDLE 94%；
  nyabula_eye 静止 1.2%、场景动画 45~64%；nyproduct 2%；nyabula_web 1.3%；adbd 0%。
- 恢复：ping 通但 nsh/adb/HTTP 全挂的那类卡死，`adb reboot` 无效，
  此前只能断电。

**边界**：

- 依赖串口中断还活着。关中断死循环里它不响应——那种仍要断电。（仅推断）
- 复位后 N-Boot 选的还是同一个槽。新固件必挂时要配合 `!` 进控制台换槽，
  见 [ota.md](ota.md)。
- 它救不了 #11 那种 eMMC 状态：强制 panic 复位后 NuttX 仍识别不了卡。

---

## 14. `nbootctl` 的分区级动词只在草稿工作树

**验证状态**：读码确认；实测日志中**无上板记录**。

`digest`、`verify-part`、`write-part`、`write-raw`、`write-gpt`、`check-raw`、
`format` 在 `tmp/amp-compute-draft-20260914/app/nbootctl/`
（`nbootctl_part.c`、`nbootctl_format.c`）。product 树
（`tmp/product-wt/app/nbootctl/`）**没有**这些动词。

反过来，结构体版读接口 `nbootctl_handoff_read()` /
`nbootctl_bootctrl_snapshot()` **只在 product 树**，草稿树没有。

**含义**：

- 在 product 固件上敲 `nbootctl write-part ...` 只会得到 usage。
- 移植是双向的：草稿树的动词要进 product，product 的库接口要进草稿。
  两边都动了 `nbootctl_main.c`，合并时预计要手工处理（仅推断，未尝试合并）。
- 草稿树 usage 文本列的分区白名单是 8 个（无 `config`），而代码里的表是 9 个
  （含 `config`，索引 8，65536 块）。以代码为准；文本是过期的。

**移植前要先看的一处（读码发现，未上板复现，仅推断）**：草稿树
`nbootctl_write_region()` 的源摘要只覆盖文件的 `bytes`，而回读
`nbootctl_verify_region()` 按**整扇区**（`take * 512`）算摘要再与同一个期望值比。
文件长度不是 512 的整数倍时，两者覆盖的字节数不同，回读比对预计恒为
`-EKEYREJECTED`——而此时数据已经写进去了。NuttX 镜像通常不是 512 对齐的
（例：4752792、4739608 都不是）。同一函数里 `memset(buffer, 0, bytes)` 也只清了
`bytes`，末扇区的尾部并没有像注释说的那样补零。MiniLoader（360448 B = 704 扇区）
与 4 MiB 的 N-Boot FIT 恰好对齐，不受影响。`check-raw` 同理只对整扇区的摘要有意义。
product 树的 `nbootctl_bootctrl_stage()` 没有这个问题：它回读时只对
`bytes` 做 `sha256update`。

动词表见 [nbootctl.md](nbootctl.md)。

---

## 上游与本下游的边界

N-Boot 是 U-Boot 的下游发行版，**保留** U-Boot 的历史与许可证。
它**新增**的是：

- KICKPI-K7 设备树与最小/完整两个 profile
- 冗余 CRC 保护的 bootctrl 记录（双域 A/B）
- `bootnuttx` / `bootamp` 两个命令
- `nboot_contract` / `nboot_storage` / `nboot_update` / `nboot_recovery`
  四个模块
- USB Fastboot 恢复及其 OEM 子命令

**这些是 clean-room 实现的**，未复制 Android 供应商 U-Boot 源码树。
DDR 初始化、ARM Trusted Firmware、OP-TEE 二进制是外部 Rockchip 组件，
不被本仓重新授权。

**提上游时注意**：clean-room 的板级与启动管理部分可以提；
DDR/ATF/OP-TEE 相关不可。
