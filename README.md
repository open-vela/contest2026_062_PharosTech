# Nyabula 星喵

**2026 首届 openvela AI 硬件开发者大赛 · Pharos Tech · 队伍 062**

本项目提交两项相互支撑的成果：**把 RK3576 / KICKPI-K7 适配为可用的原生 openvela 平台；在平台上构建能交互、能执行、能扩展的 Nyabula AI 智能硬件产品。**

| 参赛方向 | 评委可以看到什么 | 深入阅读 |
|---|---|---|
| **新硬件适配** | 从 ARM64 启动到原生无线、存储、音频、显示、摄像头编码、以太网和电源管理 | [平台适配详解](docs/platform/README.md) |
| **AI 智能硬件创新产品** | 双眼与语音、设备工具、Agent 自主创建插件、实时 DSP、双设备蓝牙、Skill / MCP / Cloud | [Nyabula 产品详解](docs/product/README.md) |

团队主线已合入产品整合及日志更新；本页同时介绍 **2026-11-04 本轮候选**，相关代码已提交至[#110](https://github.com/open-vela/contest2026_062_PharosTech/pull/110)，尚未合入。最新实机为 AMP **B109 / generation 460**，保留 A21 恢复槽；候选与主线的区别和验证范围见[版本与验证](docs/validation/README.md)。

## 第一部分 新硬件适配

### RK3576 原生 openvela 平台

我们以 openvela 已有 RK3399 ARM64 移植为参考，完成 RK3576 的启动、GICv2、MMU、时钟和外设接入。芯片层、板级层与通用设备驱动分开维护，通过 manifest 接入完整 openvela 工程。

| 平台能力 | 已交付实现 |
|---|---|
| 启动与内核 | NSH、ARM64 入口/EL、GICv2、MMU/Cache、PSCI、generic timer、SMP；N-Boot 启动恢复 |
| 基础外设 | UART、GPIO/IOMUX、CRU 时钟树、I2C、SPI、PWM、FSPI、PL330 DMA、RTC、ADC、TSADC、WDT |
| 存储与电源 | SDMMC、eMMC、ADMA2、HS200/HS400ES、GPT/FAT；RK806 regulator 与基础电源树 |
| 网络与蓝牙 | SV6621 原生 FullMAC WiFi、STA/SoftAP、WPA2/WPA3；蓝牙 HCI / ZBlue；GMAC 与 MAE0621A 千兆 PHY |
| 显示与触控 | GC9B72 双圆屏、TE 流水；VOP、HDMI、MIPI DSI、GT911 |
| 摄像头与编码 | **openvela 原生** CSI0 / VICAP / OV5647、软件 NEON 图像处理与 3A、VEPU510 H.264、camenc 浏览器推流 |
| 音频与 USB | SAI / ES8388 录放、USB Device / ADB、xHCI Host、Hub、MSC/exFAT、USB Audio；WS2812 灯带 |

**代表性结果：** eMMC 指定只读实验约 **75.5 MiB/s**；千兆以太网作者板测 **30 分钟传输 29.4 GB**；SoftAP **20 轮通过**；历史 B24 的 ADB 传输测试通过，但近期仍出现延迟 offline。完整条件和来源见[平台实测](docs/platform/README.md#代表性实测)。

显示、相机、以太网和灯带提供独立配置。当前 VOP 为 esmart0 透传，相机支持 CSI0，编码为固定 QP 的 H.264；本轮产品候选已集成原生采集与编码，B103 用户确认预览画面正常。原生 NPU 是[独立实验路线](docs/platform/README.md#原生-npu-实验路线)，尚未上板验收。

**继续阅读：** [驱动与配置地图](docs/platform/README.md) · [全部能力与状态](docs/CAPABILITIES.md) · [构建运行](docs/GETTING_STARTED.md)

## 第二部分 AI 智能硬件创新产品

### Nyabula 是一台可以不断扩展的桌面 AI 设备

双圆屏让设备状态可见，语音和浏览器让用户能直接操作。Nyabula 不只回答问题：它可以执行计时、音量和记忆等设备动作，也能让在线 Agent 在设备上生成、签名安装并迭代新的小工具。

### 现在可以做什么

| 用户任务 | 产品能力与结果 |
|---|---|
| “定一个五分钟计时器” | KWS → ASR → Nyabot / 官方 ai_agent → 设备工具 → TTS；同时驱动眼睛反馈 |
| “做一个本地记账本，再加分类筛选” | Agent 读取契约、生成源码、签名部署、发送真实 UI 事件，并根据错误反馈修复；已有三个真机生成工具 |
| “按这个 Skill 做传输时间计算器” | 导入并启用 `transfer-planning`，模型实际读取后生成并修复 GB/GiB、MB/s/Mbps 计算工具 |
| 在电脑和手机间切换音乐 | 两台蓝牙设备同时在线，选定唯一媒体源；播放、暂停、上一首、下一首已有实际验收 |
| 观察与隐私 | 摄影预览、本地人脸检测和身份登记；摄像头、麦克风独立开关与重启持久化 |
| 调整灯光 | 四区独控、瞳色同步/互补，以及动画和音乐律动 |
| 调整桌面音响 | TTS / 蓝牙 / 文件共用 16 段 EQ、实时预览与预设；板载扬声器 + USB 低音双输出、分频和热切换 |
| 从浏览器或其他 Agent 管理设备 | 本地面板、成员权限、双向 MCP、原生 Cloud 中继、二维码登录恢复 |
| 安装新功能而不重刷系统 | 签名 JS/Wasm 插件、NyaUI 组件/HTML 页面、私有数据、硬件能力授权、版本升级与恢复 |

### 产品控制在 openvela 模型计算按职责分工

**4 个 A53 运行 openvela，4 个 A72 运行 Linux。** openvela 持有状态、权限、工具执行、存储、联网、双眼和音频；Linux 提供本地 KWS/ASR/TTS、RKLLM/NPU 模型服务。MiMo 2.6 Flash / Pro 是在线文本后端，云 ASR/TTS 使用对应 2.5 系列接口。

原生相机采集与编码归 openvela；Linux 相机节点已裁剪。摄像头独立配置已交付，本轮候选已实机验证采集、编码和预览；远程媒体体验仍待联调。详见[系统架构与数据流](docs/product/ARCHITECTURE.md)。

### 产品设计的三个重点

- **执行结果可核对。** 工具权限、主人审批、步骤、实际副作用与模型回复分开显示；模型的口头承诺不能代替设备动作。
- **功能可以扩展。** 原生插件、运行时 Skill、MCP 和开发工作台共享可验证的接口；Skill 提供知识，权限仍由设备控制。
- **设备能够维护。** 模型与固件独立交付，A/B、OTA、Fastboot、CLI、Web 监听恢复和配置持久化共同支撑持续使用。

**继续阅读：** [完整产品能力与交互设计](docs/product/README.md) · [Agent 与扩展开发](docs/product/EXTENSIONS.md) · [架构](docs/product/ARCHITECTURE.md) · [实测与当前边界](docs/validation/README.md)

### 获取与运行

在 Linux 构建环境使用完整 manifest；仅克隆本仓不足以构建 openvela：

```sh
mkdir nyabula-openvela && cd nyabula-openvela
repo init -u https://github.com/open-vela/contest2026_062_PharosTech \
  -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j4
```

先选择基础 BSP、独立平台功能、普通产品或 4+4 产品配置，再按[构建运行指南](docs/GETTING_STARTED.md)准备依赖、字体、无线固件和模型资源。当前主线有 **15 套配置**，对应最后一批 CMake/Make 与其他 CI 检查均已通过。

### AI 开发与团队交付

Pharos Tech：**陈尚宏**负责队长、硬件适配与架构设计；**池泓翔**负责硬件适配与外设配件研发。具体代码与评审贡献保留 Git/PR 署名。

开发过程沉淀为 **[24 份工程 Skill](skills/README.md)**，覆盖移植、驱动、AMP、模型、插件、MCP 与交付；设备运行时的 [transfer-planning Skill](tools/nyabula_plugin/skills/transfer-planning/SKILL.md)另计。[AI Coding 日志](logs/README.md)保留来源、工具事件和继承关系，后续日志与采集范围见日志目录。

### 当前边界与来源

主线已有蓝牙音乐、实时 DSP、真机 Cloud 和运行时 Skill 实例；本轮候选增加灯光、相机、隐私、身份与耳机路由等能力。声纹模型已就绪，真人登记和匹配尚未验收；物品学习追踪B109更新面板已隐藏入口，代码候选默认关闭API和Agent入口，等待修复。真实远程画面与对讲、USB 延迟 offline、远场唤醒和原生 NPU 板测等仍待解决，按[能力清单](docs/CAPABILITIES.md)逐项标明。

N-Boot 是 U-Boot 下游；RKNN/RKLLM、无线固件、模型、字体和其他依赖遵循各自来源与许可。团队自有代码按文件中的 Apache-2.0 标记提供。源码、外部资源和镜像的交付范围见[版本与验证](docs/validation/README.md#源码与外部资源)。
