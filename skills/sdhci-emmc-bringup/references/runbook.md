# K7参数与测试矩阵

## 与SD卡host区分

K7 SDMMC使用DW-MSHC，eMMC是DW-CM-SHC/SDHCI；寄存器和DMA不可互换。K7已测卡EXT_CSD revision8、DEVICE_TYPE=0x57、STROBE_SUPPORT=1，容量31272730624 bytes。这些是特定硬件读数，不是驱动常量。

## 三个可复用故障

1. CLKCTRL的分频未改变物理输入；通过CRU设400kHz、约51.65MHz和198MHz才闭环。
2. RK3576 CMD21 tuning收到Buffer Read Ready，但没有普通Transfer Complete。等错完成条件会在128字节已到后超时。对照该IP实现，别把普通读命令也改成同样语义。
3. 块层缓冲未按cache line对齐，大请求全部PIO fallback。低地址512KiB bounce解决传输路径瓶颈，而不是删除对齐保护。

## 成功案例

团队PR54中tuning第32轮完成，HS400ES模式64MiB/512KiB请求三轮0.868/0.848/0.848秒，约75.5MiB/s；旧PIO约12.6MiB/s。重复8MiB cmp一致、末扇区61079551可读。此轮只读，不证明介质写入或掉电安全。

## 验收集合

| 阶段 | 数据/错误检查 |
|---|---|
| PIO | 单块、多块、首尾合法地址、超界拒绝 |
| ADMA2 | 对齐/不对齐、最大descriptor及boundary两侧、连续请求 |
| HS200 | DLL状态、tuning完成、固定块重复读hash |
| HS400/ES | 协商后CMD13、数据读取、模式失败恢复 |
| 生命周期 | reboot/重新初始化、错误后下一次请求 |

热重启后卡起不来而启动器仍读到GPT时，优先检查卡/host复位状态和在途写入；不是格式化分区的理由。提交通用MMC协商改动与RK时钟/控制器改动时保持分层。
