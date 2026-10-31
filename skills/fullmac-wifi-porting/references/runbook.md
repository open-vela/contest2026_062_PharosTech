# 验收矩阵与SV6621案例

## 数据通路不是裸帧透传

分别验证firmware包头、链路头、alignment/padding、credit消息和真正Ethernet载荷；控制通道、数据通道、BT通道不能共用未经分类的长度解释。复制到netdev之前校验长度，接收缓冲回收由明确所有者完成。

先检查ARP/DHCP/ICMP，再分别测DNS、TCP/UDP、广播/组播。要记录firmware中的host IP同步时刻；9月19日产品曾只在发包时同步IP，导致被动找设备间歇失败。

## 安全和恢复测试

| 场景 | 观察 |
|---|---|
| 错误密码/隐藏SSID/热点关闭 | 正确失败状态、超时、后续重新scan/connect可用 |
| WPA2、WPA3、transition | 对端实际AKM/加密/PMF、密钥安装后双向数据 |
| 重复M3/旧replay | 不重复安装旧密钥，允许协议所需重传 |
| GTK/IGTK更新 | 多轮rekey前后真实数据连通 |
| 漫游 | 同SSID不同BSSID、旧事件隔离、失败回退路径 |
| firmware assert | ready重新出现且重新建立业务；不能只看到线程没崩 |

## 已有成功与反例

团队PR60的r250实测：WPA2/WPA3 SoftAP交替20轮 `OK=20 FAIL=0`；transition分别用SAE/PSK完成数据面；rekey与约10分钟压力记录0%丢包。它不是数日可靠性或所有AP兼容性证明。

WoWLAN反例：显式suspend/resume后保持BSSID/IP并恢复ping，但休眠态magic packet为0/20，没有证明真实唤醒。接口声明与功能实测分开；不要把resume命令当GPIO唤醒。

## 每轮报告

写明芯片/firmware hash、host版本、对端/国家码/信道、STA或AP、安全模式、PHY协商与应用吞吐分别是什么、失败恢复是否跑过。日志不保留明文PSK。DFS缺CAC/radar能力时按法规/驱动策略拒绝主动发射，不靠改常量“测通”。
