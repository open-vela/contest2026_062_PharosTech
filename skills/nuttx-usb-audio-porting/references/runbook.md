# 包边界及案例

## 格式与队列

bytes/frame = channels × container bytes。采样位宽与容器位宽分开；非整frame数据应拒绝或按明确合同缓冲，不截断伪造成功。输入零长包、短包、跨缓冲尾部和partial-error已取得字节数都必须保留语义。

host的周期端点配置包含interval、Max ESIT、burst/mult。full/low-speed经高速Hub时，TT Think Time属于Hub自身slot，不复制到下游设备slot；这是K7一次Configure Endpoint `CC=17`的实际原因。

## 成功案例

NuttX fork #10/团队#90：`1b3f:2008` full-speed声卡经GL3510，节点从错误三组收敛成 `usbp0/usbc1` 一组；115.44秒48kHz/16-bit/stereo文件完整播放两次，`Play complete. outstanding=0`，用户确认实际出声；volume50/100、stop/q正常。

## 反例

当时没有适用3.5mm麦克风，Mic只有configure/start/stop/release/close返回0与宿主函数测试。不能写真实Mic录音通过；也没有证明Speaker+Mic全双工、UAC2、显式feedback或长期热拔插。

## 最小回归

- 44.1kHz非整数包与48kHz整数包分别覆盖，多于一个应用buffer。
- UAC1非连续接口、UAC2 IAD、带独立HID的composite、畸形collection。
- 音量master/逐声道、mute、Mixer/Selector分支。
- 自然EOF、显式stop、device disconnect、open句柄关闭、再次播放。
- Mic保留跨buffer数据与错误前已采字节；只有mock时注明宿主。

宿主ASan/UBSan证明解析和所有权边界，不能代替实际isochronous时序。验收报告列设备VID/PID/速度/Hub路径和每个方向结果，不只列“USB Audio通过”。
