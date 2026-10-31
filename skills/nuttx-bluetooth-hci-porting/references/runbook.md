# 能力分层及对端限制

| 层 | 最小证据 |
|---|---|
| 控制器 | ready、HCI Reset原字节、版本/地址、校准下载 |
| transport | ACL/SCO/ISO各类正确收发、credit及异常重同步 |
| BLE | uncached发现、读写/notification，central和peripheral分开 |
| Classic | 配对安全等级、link key持久化、L2CAP/RFCOMM数据 |
| A2DP/AVRCP | signaling、合法SBC、TX completion/接收帧、播放端PCM另验 |
| HFP | SLC、codec/eSCO、SCO TX与RX分别统计、真实麦/扬声器另验 |
| LE Audio | PACS/ASCS、ASE、CIG/CIS、ISO SDU与播放逐层验证 |

## SV6621成功证据

团队PR67记录Reset `04-0E-04-01-03-0C-00`、revision `0x5302`，与Windows AX211双向GATT及RFCOMM回路；大包200轮共200000 bytes上行无short send。A2DP Sink接收312 packets、2184 SBC frames、181272 bytes。源代码中service和HCI lower-half与RK板胶水分开，可迁到其他transport。

## 反例

HFP建立eSCO并发送3个60-byte合法mSBC silence payload，Windows没有真实call/audio source，未返回SCO payload。它只能证明TX，不证明电话对讲。AX211发现BAP广播并建立ACL，也没触发ASE/CIS/ISO媒体；普通WinRT GATT API不能代替原始ISO对端。

## 常见分层错误

- UART H4里遗漏SCO类型而芯片SDIO通道已实现：修host端口，别重复改firmware。
- 默认MTU为0、pool未注册、sent callback没接：建链可能成功但媒体永不前进。
- 对端缓存旧SDP/GATT：用新配对或明确uncached路径，不能拿UI缓存当空口结果。
- combo firmware复位后只恢复WiFi：Bluetooth必须报告offline/hardware error并重新NVDS/HCI初始化。

验收例：收到SBC帧但没有声音，应进入解码/PCM路由而不是宣称音箱已完成；有ISO Kconfig和clean build，仍要报告真实ISO数据面未测。
