# 分层步骤及反例

## 先画三条链

- host链：bus clock/reset、pinmux/电平、低速clock、命令发送和响应采样。
- combo链：总电源/chip enable、WiFi与BT控制脚、ROM启动模式、上电窗口时钟。
- firmware链：下载通道、包格式、输入blob版本、校准数据、服务启动事件。

host命令超时不自动证明物理断线；ready文本存在也不自动证明正式网络数据面可用。DW-MSHC与SDHCI不是同一套寄存器。

## K7成功案例

2026-07-13记录：同时补BT_RST、BT_WAKE、UART4空闲电平和上电窗口自由运行SDIO时钟后，出现：

```text
SDIOPROBE: drive=0 sample=0 CMD5 RINTSTS=00000004 RESP=90ffff00  <<< REAL RESPONSE!
```

随后RCA/function1与芯片ID `SV6160LITE`成功。组合配置有效，最小必要集合没有逐项二分；“其中一根引脚是唯一根因”没有证据。

SV6621案例的DT访问原语是function0的地址锁存加function1窗口；正式协议以固定版本的transport和protocol源码为准。不能把这组地址当所有SDIO芯片规范。

## 失败反例及取证

早先host寄存器逐项匹配Debian后仍宣称“软件穷尽”，忽略combo另一半上电环境。另有HLE控制器锁死要在CMD5前复位。每次记录最后成功的层，不把后续firmware ready失败退回成“CMD5还没解决”。

若同时改四项才恢复，保存成功集合；除非当前任务要求定位最小集，不再为了单变量原则破坏可恢复基线。

## 最小交付

提供身份/原理图网络来源，启动时序表，枚举原始响应，芯片ID，固件输入hash与ready事件，以及重复启动结果。固件缺失或仅有公共CI零占位时停止在已证实的层，不能用相同长度冒充实际固件。

验收情境：用户只给 `CMD5 -110` 时，先要原始状态/启动阶段，不先换WPA栈；当ID已成功而下载卡住，检查协议、流控与CP运行状态。
