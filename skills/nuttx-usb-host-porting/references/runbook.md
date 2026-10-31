# 阶段检查与K7案例

## attachment 与 core

团队#68提供RK3576 USB1的DWC3、USB2 PHY、Combo PHY、clock/VBUS和板级初始化；NuttX fork #9提供platform/MMIO xHCI与Hub支持。其中一部分来自Apache主线cherry-pick，复用时保留作者与来源，不称全部从零实现。

## 下一条证据

| 阶段失败 | 检查 |
|---|---|
| 控制器不响应 | MMIO/IRQ/clock/reset/PHY，实际端口VBUS |
| Hub已见、下游不见 | descriptor protocol、端口供电/复位、route/TT映射 |
| Address Device超时 | slot/EP0 context、dequeue与地址生命周期 |
| 端点配置错误 | interval、Max ESIT、burst/mult、TT所在context |
| 小包可读大包坏 | TD分片、64KiB边界、CHAIN、cache/bounce |
| 拔出后崩溃 | DMA静止、class回调寿命、event ring单消费者 |

不要同时让ISR与polling线程无协调地消费同一event ring。错误后的Disable Slot和部分初始化unwind同正常路径一样需要检查。

## 成功案例

K7 GL3510 USB2 `05e3:0610` 与USB3 `05e3:0626` Hub完整枚举；USB2读卡器exFAT挂载、读写hash、拔插恢复；ASMedia `174c:55aa` SuperSpeed/1024-byte Bulk/MaxBurst15多TRB读取通过。指定USB3 BOT路径峰值约36MiB/s，ADB Device与Host共存。

## 不能外推的反例

同盘Windows UASP读速369–382MiB/s不是NuttX UAS性能；Host和MSC通过也不证明UVC、USB MIDI、HID输入、USB串口或UAC已可用。复杂类先独立建立其描述符、端点和数据验证。

验收给定“下游full-speed设备配置失败”时，应查Hub TT context，不将全部参数按高速设备复制；给“挂载成功”时还要比较文件hash与重新插入路径。
