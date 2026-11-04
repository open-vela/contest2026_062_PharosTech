# RK3576 SPI 分频修正的板测依据

本修复仅删除额外的二分频模型，保留关闭 SSIENR 后写父时钟与 BAUDR、再恢复使能的修复。

## 同一块板的前后对照

KICKPI-K7，SPI3，33颗WS2812B，132字节颜色帧展开为1260字节SPI数据。灯珠、组偏移、颜色帧、12.5%亮度均不变。

|状态|父时钟|BAUDR|1260字节耗时|FIFO耗空|实物结果|
|---|---:|---:|---:|---:|---|
|B96，额外÷2|100 MHz|8|1074–1078 µs|19|错色、错位，全灭不完整|
|B97，source/BAUDR|100 MHz|16|1614–1615 µs|0|1红、1蓝、8黄、23白，经用户确认正确|

关键串口原文：
```
spi3_diag: words=1260 elapsed_us=1074 loop_gap_us=55 empty=19 baudr=8 ctrlr0=20c1 source_hz=100000000 timer_hz=1000000000
spi3_diag: words=1260 elapsed_us=1614 loop_gap_us=40 empty=0 baudr=16 ctrlr0=20c1 source_hz=100000000 timer_hz=1000000000
```

6.25 MHz发送1260字节至少需要1260×8/6250000=1612.8 µs。旧模型报告6.25 MHz，却在1074 µs完成，已与该报告矛盾；修正后耗时与理论值吻合。读回的报告频率在修复前后都可能相同，不能用软件报告代替线速证据。

本记录是2026-11-03 B96/B97板测。B97四组颜色由用户现场确认；生产代码已删除临时计时日志，并通过指定构建机增量编译。未增加RGBW补偿或修改灯号偏移。

## 回归

在具备C编译器的Linux环境执行：
```
python3 tools/tests/test_spi_clock.py chips/rk3576/rk3576_spi.c
```

测试直接提取生产setbaudr/getbaudr函数，验证100 MHz请求6.4 MHz得到BAUDR16、报告6.25 MHz；验证偶数分频、父钟选择、SSIENR写入顺序和零分频读回。当前主线旧÷2模型失败，修正版输出：
```
SPI_CLOCK_PASS source_over_baudr even_divisor restore_enable parent_selection
```
