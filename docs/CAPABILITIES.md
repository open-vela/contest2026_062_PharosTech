# 全部功能与能力

[参赛主页](../README.md) · [平台详解](platform/README.md) · [产品详解](product/README.md) · [验证记录](validation/README.md)

按两条赛道归类。每项分别标出交付位置和验证程度；“主线”表示源码已入仓，不等于所有场景都已通过真机验收。

## 11月4日本地候选

| 能力 | 验证与边界 |
|---|---|
| 四区灯光 | B99独控、瞳色同步/互补、动画/音乐律动、开机清零和休眠 |
| 摄影与隐私 | B103预览画面获用户确认；B105独立摄像头/麦克风开关及重启持久化通过 |
| 人脸与声纹 | B106真人检测框获确认；B108身份实际帧正常且用户登记1条人脸。声纹模型ready，真人登记/匹配未验收 |
| 耳机路由 | B108实际耳机时停用2.1，扬声器/双输出恢复；保存EQ与分频不变 |
| 物品学习追踪 | RKNN加载-5、零帧，B109更新面板已隐藏入口，代码候选默认关闭API和Agent入口，代码保留 |
| Cloud媒体 | 临时数据库中继测试通过；远程画面、双向对讲与MiMo看图未实机验收 |

## 第一部分 新硬件适配

### 平台与基础驱动

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| ARM64 启动与系统基础 | 团队主线；板测确认 | NSH、入口/EL、GICv2、MMU/Cache、PSCI、generic timer、SMP；RK3399 框架复用并适配 RK3576 | UP 历史产品不作 4+4 证明 | [#2](https://github.com/open-vela/contest2026_062_PharosTech/pull/2) · [#84](https://github.com/open-vela/contest2026_062_PharosTech/pull/84) · [源码/说明](../chips/rk3576) |
| 产品 4+4 AMP | 团队主线；板测确认 | 四 A53 运行 openvela，四 A72 运行 Linux；固定 product_amp 配置、第二堆、共享中断静态路由 | B23/B66 启动有双方 CPU 证据；不是单边可独立重启的隔离系统 | [L6907](validation/EVIDENCE.md#l6907) · [L7701](validation/EVIDENCE.md#l7701) · [#105](https://github.com/open-vela/contest2026_062_PharosTech/pull/105) · [源码/说明](../configs/product_amp/defconfig) |
| UART GPIO IOMUX CRU | 团队主线；板测确认 | UART0 早期/完整串口、五 bank GPIO、中断、时钟树/父源/分频/门控 | 模块验收按各驱动对应配置 | [#20](https://github.com/open-vela/contest2026_062_PharosTech/pull/20) · [#65](https://github.com/open-vela/contest2026_062_PharosTech/pull/65) · [#69](https://github.com/open-vela/contest2026_062_PharosTech/pull/69) · [#88](https://github.com/open-vela/contest2026_062_PharosTech/pull/88) · [源码/说明](../chips/rk3576) |
| I2C SPI PWM FSPI DMA | 团队主线；驱动与历史板测 | I2C 事务、SPI、PWM v4、FSPI/QSPI、PL330 microcode；音频/显示/存储 DMA 配套 | 接口覆盖不代表每种外部器件均测过 | [#18](https://github.com/open-vela/contest2026_062_PharosTech/pull/18) · [#32](https://github.com/open-vela/contest2026_062_PharosTech/pull/32) · [#33](https://github.com/open-vela/contest2026_062_PharosTech/pull/33) · [#36](https://github.com/open-vela/contest2026_062_PharosTech/pull/36) · [#45](https://github.com/open-vela/contest2026_062_PharosTech/pull/45) · [#46](https://github.com/open-vela/contest2026_062_PharosTech/pull/46) · [#77](https://github.com/open-vela/contest2026_062_PharosTech/pull/77) · [源码/说明](../chips/rk3576) |
| RTC SARADC TSADC timer WDT | 团队主线；驱动与历史板测 | RTC、光感ADC、温度、片载定时器和看门狗；SNTP对时与RTC回写 | 系统页四核CPU及六路温度实时展示为后续本地B90增强 | [#63](https://github.com/open-vela/contest2026_062_PharosTech/pull/63) · [#64](https://github.com/open-vela/contest2026_062_PharosTech/pull/64) · [#66](https://github.com/open-vela/contest2026_062_PharosTech/pull/66) · [#73](https://github.com/open-vela/contest2026_062_PharosTech/pull/73) · [#74](https://github.com/open-vela/contest2026_062_PharosTech/pull/74) · [源码/说明](../chips/rk3576) |
| SDMMC 与 eMMC | 团队主线；板测确认 | PIO/IDMAC、SDHCI/ADMA2、HS200 tuning、HS400ES、DMA-safe bounce、GPT/FAT、热插拔挂载 | 75.5 MiB/s 为64 MiB只读实验；双眼配置与SD引脚互斥 | [#11](https://github.com/open-vela/contest2026_062_PharosTech/pull/11) · [#37](https://github.com/open-vela/contest2026_062_PharosTech/pull/37) · [#51](https://github.com/open-vela/contest2026_062_PharosTech/pull/51) · [#54](https://github.com/open-vela/contest2026_062_PharosTech/pull/54) · [#80](https://github.com/open-vela/contest2026_062_PharosTech/pull/80) · [源码/说明](../chips/rk3576) |
| 电源与 RK806 | 团队主线；作者演示或测试记录 | NuttX regulator接入、buck/LDO开关及电压读写、K7基础电源树；作者记录全regulator读值和VCCIO SD切1.8V | 10月27日#61已合入；电源树仍有空缺，discharge等扩展未实现 | [#61](https://github.com/open-vela/contest2026_062_PharosTech/pull/61) · [源码/说明](../drivers/drivers/rk806/rk806.c) |

### 新增显示 摄像头与有线接口

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| HDMI MIPI DSI VOP 与触屏 | 团队主线；作者演示或测试记录 | HDPTXPHY、DCPHY DPHY、MIPI DSI、HDMI、VOP esmart0透传、GT911触摸及I2C修正 | 独立hdmi/mipi_dsi配置；VOP混合效果未实现；不是GPU渲染 | [#99](https://github.com/open-vela/contest2026_062_PharosTech/pull/99) · [源码/说明](../configs/hdmi/defconfig) |
| 原生 CSI0 与 OV5647 采集 | 团队主线；作者演示或测试记录 | openvela侧CSI DPHY、VICAP、OV5647、软件NEON解拜耳、曝光/增益/白平衡闭环 | 只CSI0；CSI1–4与硬件ISP未完成；本轮product_amp候选B103已实机预览 | [#100](https://github.com/open-vela/contest2026_062_PharosTech/pull/100) · [源码/说明](../boards/rk3576/kickpi-k7/src/kickpi_k7_camera.c) |
| VEPU510 硬件编码 | 团队主线；作者记录与主线构建 | NV12输入、H.264输出；V4L2编码接口、固定QP、IDR/P帧与参数集 | 不是H.265实现；无码率控制/AQ/ROI；1080高度对齐需拷贝 | [#100](https://github.com/open-vela/contest2026_062_PharosTech/pull/100) · [源码/说明](../chips/rk3576/vepu/README.md) |
| camenc 浏览器视频演示 | 团队主线；作者演示或测试记录 | 相机DQBUF→VEPU→Annex-B转fMP4→HTTP/WebSocket→浏览器MSE；模式/曝光/增益/白平衡控制 | 独立示例；Core相机产品集成与AMP联调未完成；不是全套WebRTC | [#100](https://github.com/open-vela/contest2026_062_PharosTech/pull/100) · [源码/说明](../app/camenc/README.md) |
| 千兆以太网 | 团队主线；作者演示或测试记录 | RK3576 GMAC、MAE0621A PHY；双网口回环千兆协商、ping、30分钟29.4GB TCP传输记录 | 数据来自#101作者板测；不据此推算所有网络场景吞吐 | [#101](https://github.com/open-vela/contest2026_062_PharosTech/pull/101) · [源码/说明](../chips/rk3576/rk3576_gmac.c) |
| WS2812 灯带 | 团队主线；作者演示或测试记录 | SPI3字符设备/dev/ws2812、RGB写入、真实SPI频率脉宽计算、空闲拉低与释放总线；测试程序 | 独立ws2812配置；未证明已融入猫的产品场景 | [#103](https://github.com/open-vela/contest2026_062_PharosTech/pull/103) · [源码/说明](../configs/ws2812/README.md) |

### 网络与蓝牙

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 原生 SV6621 FullMAC WiFi | 团队主线；板测确认 | 固件装载、STA/SoftAP、WPA2/WPA3-SAE、PMF、密钥更新、隐藏SSID、扫描/漫游、固件自恢复 | 20轮SoftAP及短稳态有效；WoWLAN深睡唤醒仍未完成 | [#60](https://github.com/open-vela/contest2026_062_PharosTech/pull/60) · [源码/说明](../drivers/drivers/sv6621) |
| 配网与自动回连 | 团队主线；板测确认 | SoftAP、二维码/浏览器配网、异步扫描结果、链路检测与回连、密码不进入命令历史 | 扫描/切换会影响当前连接，运维保留ADB或串口 | [L6789](validation/EVIDENCE.md#l6789) · [L6796](validation/EVIDENCE.md#l6796) · [源码/说明](../app/nyabula_core/ny_product_network.c) |
| 蓝牙底层与独立 profile | 团队主线；板测确认 | SDIO H4、ACL/SCO/ISO通道、BLE/GATT、Classic、RFCOMM/SPP、A2DP/AVRCP及HFP部分链路 | LE Audio控制面不等于ISO音频；HFP尚无完整真实电话验收 | [#67](https://github.com/open-vela/contest2026_062_PharosTech/pull/67) · [源码/说明](../app/nyabula_core/ny_product_bt.c) |
| 真实蓝牙音乐与双设备选源 | 团队主线；板测确认 | Windows与OPPO同时连接；选择唯一媒体源；暂停/恢复/上一首/下一首、元数据与独立会话状态 | B88用户确认切换无串音/爆音；OPPO首次播放需手机端手动启动一次 | [L8271](validation/EVIDENCE.md#l8271) · [#107](https://github.com/open-vela/contest2026_062_PharosTech/pull/107) · [源码/说明](../app/nyabula_core/ny_product_bt_audio.c) |
| HFP 通话与抢占框架 | 团队主线；实现、协议/宿主回归 | HFP状态、SCO音频与通话优先仲裁，双设备状态隔离 | 没有真实手机双向通话验收；通话卡空闲态不等于打电话通过 | [L7442](validation/EVIDENCE.md#l7442) · [L8271](validation/EVIDENCE.md#l8271) · [源码/说明](../app/nyabula_core/ny_product_bt.c) |

## 第二部分 AI 智能硬件创新产品与扩展

### USB 音频与 DSP

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| USB Device ADB 与恢复 | 团队主线；板测确认 | DWC3、ADB文件传输、SMP IRQ临界区、reset/ENDTRANSFER资源回收与物理拔插修复 | B24 260次拉帧及8MiB往返；近期仍延迟offline，原因未定 | [L6927](validation/EVIDENCE.md#l6927) · [L7147](validation/EVIDENCE.md#l7147) · [源码/说明](../chips/rk3576/rk3576_usb.c) |
| USB Host 与存储类 | 团队主线；板测确认 | DWC3/xHCI、USB2/3、GL3510 Hub、MSC/exFAT与自动挂载 | 历史BOT峰值约36MiB/s，非UAS；不等同新USB文件共享已实盘验收 | [#68](https://github.com/open-vela/contest2026_062_PharosTech/pull/68) · [源码/说明](../chips/rk3576) |
| SAI PL330 ES8388 录放 | 团队主线；板测确认 | 单双声道、双麦、16/24/32位、输出路由、同格式全双工与生命周期矩阵 | 66项采集中两项右路削顶保留；完整AEC/插话未验收 | [#58](https://github.com/open-vela/contest2026_062_PharosTech/pull/58) · [源码/说明](../app/audioctl) |
| USB Audio OUT 与低音设备 | 团队主线；板测确认 | USB Speaker真实播放，产品低音USB音频送流、短包节拍修复 | USB Mic IN和UAC全双工仍缺真实板测；协议已枚举不等于麦克风可录 | [L7848](validation/EVIDENCE.md#l7848) · [源码/说明](../app/nyabula_core/ny_dsp_output.c) |
| 独立 C99 DSP 算法库 | 团队主线；宿主或真实 sim 验证 | 48kHz LR4、全频立体声/2.1、独立高低通、输出增益/相位/延迟、采样峰值限幅、离线WAV工具 | 数字采样峰值保护；不是喇叭位移/温度保护或已完成箱体标定 | [#102](https://github.com/open-vela/contest2026_062_PharosTech/pull/102) · [源码/说明](../app/nyabula_dsp/README.md) |
| 三路共用实时 DSP 与 16段 EQ | 团队主线；板测确认 | TTS/蓝牙/文件共用输出；8–192kHz输入转48kHz；16段PEAK/架式EQ、实时预览/撤销/命名预设与持久化 | 旧四段ABI保留；HFP仍走8/16kHz路径 | [L7442](validation/EVIDENCE.md#l7442) · [L7477](validation/EVIDENCE.md#l7477) · [L7533](validation/EVIDENCE.md#l7533) · [源码/说明](../app/nyabula_core/ny_product_dsp.c) |
| 2.1 双输出同步与热切换 | 团队主线；板测确认 | ES8388+USB低音、队列反馈时钟校正、独立分频、热开关、软件暂停/恢复、全局音量/静音 | B76试听通过；队列差不是声学相位测量；增益/接线按验收条件 | [L7918](validation/EVIDENCE.md#l7918) · [L7930](validation/EVIDENCE.md#l7930) · [源码/说明](../app/nyabula_core/ny_dsp_output.c) |
| 流式音频供数与云 TTS 修复 | 团队主线；板测确认 | 提交长度独立记账、网络生产者与PCM播放解耦、有界队列、短间隙不插长静音、播报扩栈 | B86三轮USB欠载0，30%低音听感通过；100%输出未验收 | [L7393](validation/EVIDENCE.md#l7393) · [L8234](validation/EVIDENCE.md#l8234) · [源码/说明](../docs/voice-stream-validation-20261101.md) |

### 双眼与屏幕交互

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| GC9B72 双圆屏 | 团队主线；板测确认 | 双360×360、RGB565、FSPI、TE、双缓冲、CPU/DMA流水、NEON与预烘焙优化 | 40→4ms为指定1.8GHz渲染实验；不作整机满载60fps保证 | [#75](https://github.com/open-vela/contest2026_062_PharosTech/pull/75) · [#86](https://github.com/open-vela/contest2026_062_PharosTech/pull/86) · [L6907](validation/EVIDENCE.md#l6907) · [源码/说明](../app/nyabula_display) |
| Eye 表情与场景引擎 | 团队主线；板测确认 | 13类表情、25类历史Scene、眨眼/凝视/虹膜/异瞳、字幕、优先级/lease/release | 场景渲染、数据绑定与硬件传感器是三个不同完成层 | [#85](https://github.com/open-vela/contest2026_062_PharosTech/pull/85) · [#94](https://github.com/open-vela/contest2026_062_PharosTech/pull/94) · [源码/说明](../app/nyabula/SCENE_SCHEMA.md) |
| 中文字体与实际屏幕镜像 | 团队主线；板测确认 | 静态GB2312字库、认证/eyes/stream、配对QR中文页、eyeprobe与无屏软件TE配置 | 普通产品硬TE；免登录eyeprobe仅受控无屏演示配置 | [L6757](validation/EVIDENCE.md#l6757) · [L6907](validation/EVIDENCE.md#l6907) · [L7020](validation/EVIDENCE.md#l7020) · [源码/说明](../configs/product_judge/defconfig) |
| 光感瞳孔与任务反馈 | 团队主线；参数板测、场景回归 | ADC触发/平滑/极性、瞳孔参数；Agent显示真实步骤与不定进度 | 最新开场眼皮/实体首帧目视仍有独立待验项 | [L6775](validation/EVIDENCE.md#l6775) · [L7631](validation/EVIDENCE.md#l7631) · [源码/说明](../app/nyabula_core/ny_product_light.c) |
| 设备端持续场景绑定 | 代码PR #110；板测确认 | B93/B94直接从服务读天气/音乐/音频/EQ/网络/系统等；秒表无需浏览器推送仍前进；清除演示值回退 | 载荷与代码回归通过；实体像素和设备整页最终目视未完成 | [L8426](validation/EVIDENCE.md#l8426) · [L8442](validation/EVIDENCE.md#l8442) |

### AI 与语音

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 端侧 MiniCPM5 Agent | 团队主线；板测确认 | RKLLM 1.3 W4A16、本地闲聊、规则意图/单工具补参、实际时间/计时/音量/表情/记忆动作 | 25–26tok/s为指定短请求历史实验；不能推广所有模型 | [#105](https://github.com/open-vela/contest2026_062_PharosTech/pull/105) · [源码/说明](../app/nyabula_core/ny_agent_local.c) |
| 独立 tokenizer 与模型前端 | 团队主线；宿主或真实 sim 验证 | C++17 byte BPE、Unicode、chat template、流式UTF8、工具解析；293golden/30293fuzz参考对照 | 对应MiniCPM5前端，不是任意模型通用正确性 | [#87](https://github.com/open-vela/contest2026_062_PharosTech/pull/87) · [源码/说明](../tools/amp/chat/README.md) |
| MiMo 2.6 Flash 与 Pro | 团队主线；板测确认 | 真实在线对话与工具；普通API/Token Plan配置、模型路由、思考偏好 | 九月2.5-pro的14/27秒不写成2.6性能 | [L7549](validation/EVIDENCE.md#l7549) · [L7557](validation/EVIDENCE.md#l7557) · [源码/说明](../app/nyabula_core/ny_mimo.c) |
| 端侧 KWS ASR TTS 与整回合 | 团队主线；板测确认 | 预录、流式KWS/ASR、Nyabot、G2P/Melo、PCM播放、取消/超时/状态和眼睛联动 | 默认说话闭麦；真人唤醒率和远场识别仍不足；完整链曾现场确认 | [L6736](validation/EVIDENCE.md#l6736) · [#105](https://github.com/open-vela/contest2026_062_PharosTech/pull/105) · [源码/说明](../app/nyabula_core/ny_voice.c) |
| 云 ASR 与云 TTS | 团队主线；协议、板端链路与播报板测 | mimo-v2.5-asr/tts接入原生语音链，异步所有权和取消；B86长句流式播报 | 4秒固定参考ASR约1.469秒是PC请求；实际麦克风不能据此宣称准确 | [L7557](validation/EVIDENCE.md#l7557) · [L8234](validation/EVIDENCE.md#l8234) · [源码/说明](../app/nyabula_core/ny_voice_cloud.c) |
| 云视觉请求能力 | 团队主线；协议实现与夹具 | 图片请求编码与MiMo视觉模型配置 | 早期真实请求429；未找到后续图片理解成功的充分板测，不列完成体验 | [L6999](validation/EVIDENCE.md#l6999) · [源码/说明](../app/nyabula_core/ny_mimo.c) |
| 独立唤醒配置与测试 | 代码PR #110；板测确认 | 附加自定义词、灵敏度、启停、1000ms唤醒/麦克风测试与原状态恢复；默认赛事词保留 | B92接口通过；真人低命中尚未解决，滤波只做离线对照 | [L8326](validation/EVIDENCE.md#l8326) · [L8334](validation/EVIDENCE.md#l8334) |

### Agent 创作与运行时 Skill

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 设备 Agent 自主生成插件 | 团队主线；板测确认 | 读取契约→生成→设备签名安装→实际UI事件测试→错误回传→自修复；记账本、传输计算器、设备体检三个实例 | 真实模型源码而非外部代写；部分run仍有失败步骤或口头完成 | [L7593](validation/EVIDENCE.md#l7593) · [L7631](validation/EVIDENCE.md#l7631) · [L7669](validation/EVIDENCE.md#l7669) · [源码/说明](../docs/agent-workbench.md) |
| 事务化更新与能力限制 | 团队主线；sim与板测 | agent-*命名空间、设备Ed25519签名、baseVersion检查、失败恢复代码/数据/授权、首次有效页面验证 | 自动安装为明确开关；生成不能代替主人批准更高权限 | [L7701](validation/EVIDENCE.md#l7701) · [源码/说明](../app/nyabula_core/ny_author.c) |
| 运行时 SKILL.md 导入与消费 | 团队主线；板测确认 | UTF8导入/预览、8192字节正文、保存后停用、主人启用、按需分页读取；transfer-planning真实使用 | 与24份工程Skill分开计数；Skill文本不授予工具权限 | [L7641](validation/EVIDENCE.md#l7641) · [L7658](validation/EVIDENCE.md#l7658) · [源码/说明](../tools/nyabula_plugin/skills/transfer-planning/SKILL.md) |
| 执行审批与真实执行记录 | 团队主线；板测确认 | 真实MiMo请求pending、允许后精确写入、拒绝后不写；展示步骤/决策/sideEffectApplied | B77存在零tool_calls口头承诺；取消分支本轮未得到pending实机覆盖 | [L7991](validation/EVIDENCE.md#l7991) · [源码/说明](../app/nyabula_core/ny_agent.c) |
| 会话 记忆 自动化 Node 与渠道 | 团队主线；实现及主机/sim，部分板测 | 会话/历史/取消、记忆与profile、自动化任务、Node、飞书/微信渠道接入 | 分通道核验；不能把mock渠道测试当真实账号收发 | [#91](https://github.com/open-vela/contest2026_062_PharosTech/pull/91) · [#106](https://github.com/open-vela/contest2026_062_PharosTech/pull/106) · [源码/说明](../app/nyabula_core/ny_agent_channels.c) |
| 定时与心跳自动化 | 团队主线；实现及主机/sim回归 | at一次性、every间隔、heartbeat心跳；保存/启停/删除/立即运行及状态更新 | 未在本轮取得所有调度模式的长时间真机验收，不扩大为任意cron语法 | [#91](https://github.com/open-vela/contest2026_062_PharosTech/pull/91) · [#106](https://github.com/open-vela/contest2026_062_PharosTech/pull/106) · [源码/说明](../app/nyabula_core/ny_agent_automation.c) |

### Core 插件与权限

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| JS Wasm 插件运行时 | 团队主线；板测确认 | QuickJS/WAMR、provider/Broker、Ed25519、撤销、.nya打包、版本槽/健康/配额、异步调度 | 可信本地插件模式；无任意恶意代码强沙箱 | [#85](https://github.com/open-vela/contest2026_062_PharosTech/pull/85) · [L7189](validation/EVIDENCE.md#l7189) · [源码/说明](../app/nyabula_core) |
| 插件安装与交互生命周期 | 团队主线；板测确认 | 浏览器分块上传、中文名、列表启停、签名拒绝、升级/回滚、真实组件/HTML事件、重启存储 | 包验签和角色不足错误文案历史上曾混淆 | [L7288](validation/EVIDENCE.md#l7288) · [L7217](validation/EVIDENCE.md#l7217) · [源码/说明](../app/nyabula_core/ny_plugins.c) |
| NyaUI 多视图与 HTML 容器 | 团队主线；板测与浏览器实跑 | 组件树、HTML iframe、宿主切换、多视图事件、透明/自动高度、主题消息、PNG/iconfont | HTML隔离与声明事件；不等于完整浏览器插件执行权限 | [L7288](validation/EVIDENCE.md#l7288) · [L7352](validation/EVIDENCE.md#l7352) · [源码/说明](../app/nyabula_core/ny_ui.c) |
| owner family guest 访问控制 | 团队主线；板测确认 | 随机成员凭据摘要、中央授权、角色变更/撤销使旧会话失效、guest读/family操作/owner管理 | 真实LAN握手与撤权实测；权限在后端执行 | [L7210](validation/EVIDENCE.md#l7210) · [源码/说明](../app/nyabula_core/ny_access.c) |
| 硬件能力授权 | 团队主线；板测确认 | 设备/音频/蓝牙/USB只读及受控操作；插件事件遵循实际身份与声明权限 | 未授权不能因页面显示而获得能力 | [L7352](validation/EVIDENCE.md#l7352) · [L7669](validation/EVIDENCE.md#l7669) · [源码/说明](../app/nyabula_core/ny_hardware.c) |

### 原生产品服务

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 计时与记录 | 团队主线；sim与部分板测 | 倒计时、秒表、睡眠计时、多闹钟/重复、日历、任务、记忆、SQLite/CAS持久化 | 主线服务存在；B94选中记录到眼屏的持续绑定另列 | [#91](https://github.com/open-vela/contest2026_062_PharosTech/pull/91) · [L7041](validation/EVIDENCE.md#l7041) · [源码/说明](../app/nyabula_core/ny_product_timers.c) |
| 天气 简报与主动陪伴 | 团队主线；实现、软件回归与公开查询板测 | 和风天气/Open-Meteo、当前/7日/24小时、缓存/错误、简报、通知去重、主动陪伴设置 | B94金州真实配置和场景数据在本地；不是感知情绪或健康 | [L7612](validation/EVIDENCE.md#l7612) · [#106](https://github.com/open-vela/contest2026_062_PharosTech/pull/106) · [源码/说明](../app/nyabula_core/ny_product_weather.c) |
| 音乐 媒体与音频控制 | 团队主线；板测确认 | 文件播放、暂停/恢复、路径选择、音量/静音、输出/输入路由、DSP状态和设备仲裁 | HFP真实通话与全部媒体并发组合仍待验收 | [L7918](validation/EVIDENCE.md#l7918) · [源码/说明](../app/nyabula_core/ny_product_media.c) |
| 时间与设备状态 | 团队主线；板测确认 | SNTP、RTC启动读取/回写、系统/存储/网络/端点状态 | 四核CPU统计与TSADC六路温度API修正为本地B90 | [L6693](validation/EVIDENCE.md#l6693) · [源码/说明](../app/nyabula_core/ny_product_timesync.c) |
| USB 文件共享 | 团队主线；宿主或真实 sim 验证 | FAT/exFAT USB MSC、认证HTTP浏览/目录/上传/下载、只读默认、范围下载和短期单路径凭据 | 实物USB盘完整板测尚缺；不是SMB，不接触eMMC | [#106](https://github.com/open-vela/contest2026_062_PharosTech/pull/106) · [源码/说明](../app/nyabula_core/ny_nas.c) |
| 管理 CLI 与登录恢复 | 团队主线；板测确认 | nyabula菜单/命令、网络/模型/语音/状态管理；交互重置密码、旧会话失效、五分钟QR显示/退出 | QR不送云模型；串口/ADB属于有权运维入口 | [L6992](validation/EVIDENCE.md#l6992) · [L7020](validation/EVIDENCE.md#l7020) · [源码/说明](../tools/nyabula_core/cli.md) |

### MCP Cloud 与外部连接

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 原生出站 MCP | 团队主线；板测确认 | HTTPS发现工具、具体参数逐次审批、schema变化重确认、远端结果不确定不自动重试 | 已有公共HTTPS工具调用；不是所有第三方server兼容保证 | [L7612](validation/EVIDENCE.md#l7612) · [源码/说明](../app/nyabula_core/ny_agent_mcp.c) |
| 原生入站 MCP | 团队主线；板测确认 | loopback+隧道、限期scope凭据、工具裁剪、隔离聊天、撤销即时拒绝 | B62真实401/initialize/时钟读/撤销验证；不默认暴露LAN | [L7593](validation/EVIDENCE.md#l7593) · [源码/说明](../app/nyabula_core/ny_agent_mcp_in.c) |
| 微信与飞书适配 | 团队主线；宿主或真实 sim 验证 | iLink扫码登录/消息收发协议、飞书适配、同一Agent入口 | 没有实际微信扫码收发/真实外部账号完整联调结论 | [L7583](validation/EVIDENCE.md#l7583) · [#106](https://github.com/open-vela/contest2026_062_PharosTech/pull/106) · [源码/说明](../app/nyabula_core/ny_agent_weixin.c) |
| 原生 Cloud 客户端 | 团队主线；板测确认 | K7主动注册、设备认领/密码认证、表情/注视中继、重启状态恢复和断线重连 | 已是真机链路，不再仅Simulator；测试服务部署在本机 | [L7089](validation/EVIDENCE.md#l7089) · [L7096](validation/EVIDENCE.md#l7096) · [源码/说明](../app/nyabula_core/ny_cloud.c) |
| Cloud WSS 认证 | 团队主线；板测确认 | 正确证书连接、错误主机名拒绝、恢复正确证书后自动重连 | 本机测试CA/SAN实测；公网生产部署、账号运营及长稳另验 | [L7082](validation/EVIDENCE.md#l7082) · [源码/说明](../app/nyabula_core/ny_cloud.c) |
| Go Cloud 服务与 Console | 工作区独立工程；Console亦在主线；板测确认 | Go/SQLite账号/认领/统计、中继与静态Console；原生K7控制实跑 | Go Cloud源码未位于团队当前tree；Cloud终止TLS，不是端到端加密 | [L7089](validation/EVIDENCE.md#l7089) · [L7096](validation/EVIDENCE.md#l7096) |

### 设备 Web 与开发生态

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 设备自托管 WebUI | 团队主线；板测确认 | 桌面/平板/手机、认证HTTP/WebSocket/NyaLink、设备/插件/Nyabot/功能页、配网和升级 | 前端ready标签不是所有硬件功能已验收的依据 | [#107](https://github.com/open-vela/contest2026_062_PharosTech/pull/107) · [L7991](validation/EVIDENCE.md#l7991) · [源码/说明](../app/nyabula_web) |
| 经典 星云 仪器三皮肤 | 团队主线；真实设备操作与浏览器验收 | 多主题和深浅色、布局、入场、权限队列、返回和执行记录；日光已迁移回经典 | AGENTS十月17日未联调描述已被后续成果更新；design-lab仍非产品 | [L7033](validation/EVIDENCE.md#l7033) · [L7477](validation/EVIDENCE.md#l7477) · [L7991](validation/EVIDENCE.md#l7991) · [源码/说明](../app/nyabula_web/apps/nyabula/src/skins) |
| Web 服务内部恢复 | 团队主线；板测确认 | accept/监听错误退避重建、主动web-stop保持停止、复用凭据与会话代次、断连抑制SIGPIPE | B78实机16次HTTP中断与停启通过；不覆盖强杀任务/内存破坏 | [L8102](validation/EVIDENCE.md#l8102) · [源码/说明](../app/nyabula_core/ny_web.c) |
| Windows 开发工作台 | 工作区独立工程；宿主或真实 sim 验证 | Goja真实插件、组件/HTML/JSON视图、身份/权限/存储/日志/故障、设备夹具、19项开发MCP、HTTP/stdio | 与板上QuickJS不同；10/23新EXE交互启动因当时权限未验收 | [L7245](validation/EVIDENCE.md#l7245) · [L7713](validation/EVIDENCE.md#l7713) |
| 插件 SDK 与示例 | 团队主线；构建、主机/板测 | TS/C/Rust/TinyGo接口，签名打包；记账、传输规划、硬件面板与资源示例 | 不同语言运行时/构建证据分开，不宣称全平台发布 | [#85](https://github.com/open-vela/contest2026_062_PharosTech/pull/85) · [L7713](validation/EVIDENCE.md#l7713) · [源码/说明](../tools/nyabula_plugin) |
| Flutter NyaLink NyaUI 独立产出 | 工作区独立工程；宿主或真实 sim 验证 | Flutter各平台工程、Windows构建/42tests；Go/TS/Flutter协议适配、NyaUI组件树与patch DSL | 历史原型独立列出，非四平台均发版或主线全包含 | [历史实验](validation/README.md#仍需独立验收) |

### 启动 发布与复现

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| N-Boot 与 nbootctl | 主线客户端 + 独立 N-Boot 分支；板测确认 | 双介质、NuttX/AMP槽、bootctrl、校验、Fastboot、stage/clone/confirm、handoff与OTA | N-Boot #9/#10/#11仍OPEN；不能说最新启动器全已进release | [N-Boot #9](https://github.com/BeaconCat/N-Boot/pull/9) · [N-Boot #10](https://github.com/BeaconCat/N-Boot/pull/10) · [N-Boot #11](https://github.com/BeaconCat/N-Boot/pull/11) · [源码/说明](../app/nbootctl/README.md) |
| 配置 数据与模型交付 | 团队主线；板测确认 | 独立/config与/data、资源bundle、分块模型上传、BLOB共享搬运与缓存、启动首建文件系统 | 模型/runtime/固件分离；来源和第三方分发条件仍须按包核实 | [#104](https://github.com/open-vela/contest2026_062_PharosTech/pull/104) · [源码/说明](../docs/amp-model-delivery.md) |
| 普通和 AMP OTA | 团队主线；板测确认 | 面板上传、非运行槽保护、摘要检查、激活/重启/确认；A21恢复流程 | 写运行槽、启动器原位升级与断电故障各有独立限制 | [L6693](validation/EVIDENCE.md#l6693) · [#104](https://github.com/open-vela/contest2026_062_PharosTech/pull/104) · [源码/说明](../app/nyabula_core/ny_web_ota.c) |
| 15套配置 Make 与 CMake | 团队主线；CI全通过 | nsh/amp、camera/core_eye/core_services/dev/display/ethernet/hdmi/mipi_dsi/product/product_amp/judge两套/ws2812；工作区与补丁入口 | G2P部分测试依外部资源跳过；CI占位无线固件不能作联网镜像 | [#107](https://github.com/open-vela/contest2026_062_PharosTech/pull/107) · [源码/说明](../tools/setup_workspace.sh) |

### 最新本地增强

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| CPU 温度与启动换行修正 | 代码PR #110；板测确认 | 四核CPU统计、TSADC六路温度、ASCII板型、initramfs无扩展名启动文件LF检查 | B90 API板测；不混同主线已经含修正 | [L8308](validation/EVIDENCE.md#l8308) |
| B94 真实天气与场景载荷 | 代码PR #110；板测确认 | 金州区Open-Meteo、配置穿重启；音量/网络/EQ/低音/字幕使用实际载荷 | 天气为模型估计；最终实体像素验收未完成；六种感知场景缺真实来源 | [L8442](validation/EVIDENCE.md#l8442) |
| 页面更新提示与开场细节 | 代码PR #110；组件渲染与前端回归 | 更新提示覆盖8外壳、3皮肤深浅色及360px；瞳色缓存、光晕/眨眼交接调整 | 设备整页最终截图和偶发眼皮问题未完成验收 | [L8436](validation/EVIDENCE.md#l8436) · [L8349](validation/EVIDENCE.md#l8349) |

### 研究与方法资产

| 能力 | 交付与验证 | 已实现内容 | 当前范围 | 入口 |
|---|---|---|---|---|
| 原生 NPU 驱动路线 | 独立feat/native-npu；未上板；实现与编译 | 驱动、Mesa MIT卷积编码器移植、矩阵乘映射、/dev/npu0与rknpu自测；7形状逐字节/元素对照 | 不是已跑通原生NPU，更不是原生LLM；与Linux NPU资源不混用 | [L6848](validation/EVIDENCE.md#l6848) |
| Linux 摄像头 SFace 与多模型实验 | 历史独立实验；历史独立板测 | 1080P RAW900帧、SFace FP16数值/延迟、三模型同驻资源数据 | Linux相机链已退出产品职责；不据旧实验宣称现在产品认主 | [历史实验](validation/README.md#仍需独立验收) |
| 24份工程 Skill | 团队主线；结构/脚本与包验证 | 平台、驱动、AMP、模型、插件、面板、MCP与复现的任务入口/反例/脚本 | 对应9月交付版本；相机归属/新DSP等经验需同步，不能默认全是当前操作指南 | [#95](https://github.com/open-vela/contest2026_062_PharosTech/pull/95) · [源码/说明](../skills/README.md) |
| AI Coding 日志 | 团队主线；官方格式校验与来源记录 | 已合入双方历史日志；BeaconCat11会话11848事件，含工具/继承关系说明 | 后续日志 #109 已合入；事件数不是token总量或代码占比 | [#96](https://github.com/open-vela/contest2026_062_PharosTech/pull/96) · [#98](https://github.com/open-vela/contest2026_062_PharosTech/pull/98) · [#109](https://github.com/open-vela/contest2026_062_PharosTech/pull/109) · [源码/说明](../logs) |
| 机械 品牌 虚拟猫舍与媒体 | 工作区独立资产；模型/浏览器/历史实验 | 猫身/后盖/STL/抽壳，Logo/SVG，Three.js猫舍及音乐探索 | 不扩张为最终装配、量产、板上GPU/VR或已完成运动机器人 | [历史实验](validation/README.md#仍需独立验收) |

## 交付口径

纯平台能力中的音频/USB/双圆屏，在第二部分还体现为产品流水线；它们的驱动入口见平台详解，产品集成证据见对应行。独立工程、原生NPU和历史Linux视觉实验保持单列。

本轮候选与团队主线的区别及最新设备证据见[验证记录](validation/README.md#最新本地候选)。
