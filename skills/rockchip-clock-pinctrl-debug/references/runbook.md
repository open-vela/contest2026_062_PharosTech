# 判别与案例

| 现象 | 优先检查 | 能排除什么 |
|---|---|---|
| 第一次访问IOC就停止输出 | pclk/PD、安全域、MMU属性、访问宽度、最后异常地址 | 不先把无日志等同CPU没启动 |
| set_rate成功而吞吐不变 | 最终父源/分频；请求是否实际走DMA；功能时钟是否由另一CRU路径决定 | 不能用吞吐单独判物理频率 |
| gate打开后很快关闭 | 引用计数、unused-clock清理、另一OS或驱动是否仍拥有它 | 不能一律改成永久常开 |
| GPIO mux对但总线不回应 | pull、drive、schmitt、电压、控制脚时序及对端状态 | 不能据mux对宣告引脚链完成 |

## K7 eMMC 的成功对照

团队PR54记录：SDHCI CLKCTRL分频没有改变RK3576 dwcmshc物理源时钟，实际仍是loader的24MHz。显式配置CRU CLKSEL_CON89后，识别模式为xin24m/60=400kHz，高速为GPLL/23约51.65MHz，HS200/400为GPLL/6=198MHz。该组寄存器是RK3576案例，别套其他SoC。

性能还受DMA对齐影响：时钟改对而块请求全走PIO时，不能认为时钟修改失败。把“设频”“数据路径”“吞吐”分开取证。

## 反例：只有一个软件视图

媒体实验中CPUFreq/SCMI与本地CCF曾给出不同频率。保留原始读数和来源；不以其中一个值单独证明PLL已改，也不把PC测得的性能比搬到板上。

## 可复用记录

每个节点记录 `owner / parent / source-rate / divider / effective-rate / gate / reset / PD / observed-at`；每个引脚记录 `mux / pull / drive / voltage-source / board-net`。只读快照工具须避开read-clear或具有副作用的寄存器。

## 验收情境

- 输入：CLKCTRL写入成功、传输仍慢。输出应包含CRU与实际PIO/DMA路径两个分支，不直接提高目标频率。
- 输入：热重启失败、断电成功。输出应比较复位覆盖范围及器件状态，而不是删除文件系统或不断重复裸写。
