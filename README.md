# Nyabula 星喵

![Nyabula 星喵：新硬件适配与 AI 智能硬件创新产品](docs/assets/nyabula-hero.png)

**2026 首届 openvela AI 硬件开发者大赛 · Pharos Tech · 队伍 062**

Nyabula 是一只基于 KICKPI-K7 的桌面 AI 陪伴猫：双圆屏表达状态，语音和浏览器连接日常操作，Agent 可以调用设备工具，也可以按需求生成新的插件。

这件作品从底层做到产品：一条线将 **RK3576 移植到 openvela**，另一条线将驱动、模型和设备服务组织成可使用、可扩展的 **Nyabula**。

| 参赛方向 | 交付内容 | 深入阅读 |
|---|---|---|
| **新硬件适配** | ARM64 启动、原生无线、存储、音频、显示、摄像头编码、以太网与电源管理 | [平台适配详解](docs/platform/README.md) |
| **AI 智能硬件创新产品** | 双眼与语音、Agent 工具执行、插件生成、实时 DSP、Skill / MCP / Cloud | [产品技术说明](docs/product/README.md) |

[平台适配](#第一部分-新硬件适配) · [产品与界面](#第二部分-ai-智能硬件创新产品) · [构建运行](#获取与运行) · [验证记录](docs/validation/README.md)

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

**代表性结果：** eMMC 指定只读实验约 **75.5 MiB/s**；千兆以太网作者板测 **30 分钟传输 29.4 GB**；SoftAP **20 轮通过**；B24 的 ADB 传输测试通过。完整条件和来源见[平台实测](docs/platform/README.md#代表性实测)。

显示、相机、以太网和灯带提供独立配置。当前 VOP 为 esmart0 透传，相机支持 CSI0，编码为固定 QP 的 H.264；产品已集成原生采集与编码，B103 用户确认预览画面正常。原生 NPU 是[独立实验路线](docs/platform/README.md#原生-npu-实验路线)，尚未上板验收。

**继续阅读：** [驱动与配置地图](docs/platform/README.md) · [全部能力与状态](docs/CAPABILITIES.md) · [构建运行](docs/GETTING_STARTED.md)

## 第二部分 AI 智能硬件创新产品

### 一套设备服务，多种交互入口

可以说话，可以点开面板，也可以让外部 Agent 通过 MCP 操作。计时、记忆、权限和执行结果留在设备上，各入口使用同一套服务；关闭浏览器不会把设备变成一张停止运行的网页。

产品采用 **4 个 A53 运行 openvela，4 个 A72 运行 Linux**。openvela 管理状态、权限、工具、音频、双眼，以及原生相机采集与编码；Linux 计算域承载 KWS / ASR / TTS 和 RKLLM / NPU 模型服务。[系统架构与数据流](docs/product/ARCHITECTURE.md)说明两侧如何协作。

从主页看设备状态，从功能页直接操作，也可以在 Nyabot 里描述要做的事。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>主页</strong><br><a href="docs/assets/screenshots/20261104/nebula/home.png"><img src="docs/assets/screenshots/20261104/nebula/home.png" alt="主页，nebula主题" width="100%"></a></td>
<td valign="top"><strong>主页</strong><br><a href="docs/assets/screenshots/20261104/classic/home.png"><img src="docs/assets/screenshots/20261104/classic/home.png" alt="主页，classic主题" width="100%"></a></td>
<td valign="top"><strong>主页</strong><br><a href="docs/assets/screenshots/20261104/instrument/home.png"><img src="docs/assets/screenshots/20261104/instrument/home.png" alt="主页，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>功能</strong><br><a href="docs/assets/screenshots/20261104/nebula/features.png"><img src="docs/assets/screenshots/20261104/nebula/features.png" alt="功能，nebula主题" width="100%"></a></td>
<td valign="top"><strong>功能</strong><br><a href="docs/assets/screenshots/20261104/classic/features.png"><img src="docs/assets/screenshots/20261104/classic/features.png" alt="功能，classic主题" width="100%"></a></td>
<td valign="top"><strong>功能</strong><br><a href="docs/assets/screenshots/20261104/instrument/features.png"><img src="docs/assets/screenshots/20261104/instrument/features.png" alt="功能，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

下面每行展示同一页面的三种主题，点击图片可查看完整尺寸。主体截图来自 **B109 实机与同期面板**；Cloud 账号与高级更新页由本地模拟器补拍，已在图注标明。敏感字段已遮盖，摄影预览关闭。各项实测条件见[验证记录](docs/validation/README.md)。

[语音与双眼](#语音与双眼让交互有回应) · [音乐与调音](#从蓝牙音乐到三路音频输出) · [灯光](#让灯光跟随设备状态) · [摄影与隐私](#看见人也把隐私开关交给人) · [日常工具](#让计时提醒和记录留在设备上) · [扩展](#让-agent-给设备增加工具) · [连接与维护](#从本地连接到远程访问) · [主题与屏幕](#同一套功能适应不同屏幕)

### 语音与双眼，让交互有回应

语音回合按 **唤醒 → 识别 → Agent 处理 → 工具执行或回答 → 合成播报** 推进。Nyabot 将模型回复与设备执行步骤分开展示：用户能检查工具是否真的调用、是否需要批准，而不只听到一句“已经完成”。本地文本使用 MiniCPM5-1B / RKLLM；在线文本可选择 MiMo 2.6 Flash / Pro，云语音使用对应的 2.5 系列接口。

唤醒设置保留赛事词“你好，openvela / Hello, openvela”，并支持附加词、灵敏度与独立测试。当前默认播报时闭麦；远场唤醒质量、全双工 AEC 与随时插话仍待完善。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>Nyabot</strong><br><a href="docs/assets/screenshots/20261104/nebula/agent.png"><img src="docs/assets/screenshots/20261104/nebula/agent.png" alt="Nyabot，nebula主题" width="100%"></a></td>
<td valign="top"><strong>Nyabot</strong><br><a href="docs/assets/screenshots/20261104/classic/agent.png"><img src="docs/assets/screenshots/20261104/classic/agent.png" alt="Nyabot，classic主题" width="100%"></a></td>
<td valign="top"><strong>Nyabot</strong><br><a href="docs/assets/screenshots/20261104/instrument/agent.png"><img src="docs/assets/screenshots/20261104/instrument/agent.png" alt="Nyabot，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>语音唤醒</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-wake.png"><img src="docs/assets/screenshots/20261104/nebula/feature-wake.png" alt="语音唤醒，nebula主题" width="100%"></a></td>
<td valign="top"><strong>语音唤醒</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-wake.png"><img src="docs/assets/screenshots/20261104/classic/feature-wake.png" alt="语音唤醒，classic主题" width="100%"></a></td>
<td valign="top"><strong>语音唤醒</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-wake.png"><img src="docs/assets/screenshots/20261104/instrument/feature-wake.png" alt="语音唤醒，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

双眼不仅用于装饰。表情、凝视、虹膜与异瞳可手动调整，计时和工具运行等场景则由设备服务驱动；高优先级场景结束后释放自己的占用。字幕入口可把文字送到双屏。持续场景已验证脱离浏览器运行，全部实体像素效果仍按场景逐项验收。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>眼睛控制</strong><br><a href="docs/assets/screenshots/20261104/nebula/eye.png"><img src="docs/assets/screenshots/20261104/nebula/eye.png" alt="眼睛控制，nebula主题" width="100%"></a></td>
<td valign="top"><strong>眼睛控制</strong><br><a href="docs/assets/screenshots/20261104/classic/eye.png"><img src="docs/assets/screenshots/20261104/classic/eye.png" alt="眼睛控制，classic主题" width="100%"></a></td>
<td valign="top"><strong>眼睛控制</strong><br><a href="docs/assets/screenshots/20261104/instrument/eye.png"><img src="docs/assets/screenshots/20261104/instrument/eye.png" alt="眼睛控制，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>字幕</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-caption.png"><img src="docs/assets/screenshots/20261104/nebula/feature-caption.png" alt="字幕，nebula主题" width="100%"></a></td>
<td valign="top"><strong>字幕</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-caption.png"><img src="docs/assets/screenshots/20261104/classic/feature-caption.png" alt="字幕，classic主题" width="100%"></a></td>
<td valign="top"><strong>字幕</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-caption.png"><img src="docs/assets/screenshots/20261104/instrument/feature-caption.png" alt="字幕，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 从蓝牙音乐到三路音频输出

Nyabula 可作为桌面音箱使用：两台蓝牙设备可以同时在线，再选择一个媒体来源；也能播放设备 `/data/music` 中的本地曲目。双设备切源、播放控制和实际试听已完成板测，两个来源的元数据与按键事务分别维护。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>蓝牙音频</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-bluetooth.png"><img src="docs/assets/screenshots/20261104/nebula/feature-bluetooth.png" alt="蓝牙音频，nebula主题" width="100%"></a></td>
<td valign="top"><strong>蓝牙音频</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-bluetooth.png"><img src="docs/assets/screenshots/20261104/classic/feature-bluetooth.png" alt="蓝牙音频，classic主题" width="100%"></a></td>
<td valign="top"><strong>蓝牙音频</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-bluetooth.png"><img src="docs/assets/screenshots/20261104/instrument/feature-bluetooth.png" alt="蓝牙音频，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>音乐</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-music.png"><img src="docs/assets/screenshots/20261104/nebula/feature-music.png" alt="音乐，nebula主题" width="100%"></a></td>
<td valign="top"><strong>音乐</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-music.png"><img src="docs/assets/screenshots/20261104/classic/feature-music.png" alt="音乐，classic主题" width="100%"></a></td>
<td valign="top"><strong>音乐</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-music.png"><img src="docs/assets/screenshots/20261104/instrument/feature-music.png" alt="音乐，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

TTS、A2DP 和文件播放共用 **48 kHz DSP 出口**。先在音频路由中选择扬声器或耳机，再用 16 段均衡器调整声音；单段支持钟形、低架与高架，提供实时预览、撤销、保存和命名预设。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>音频路由</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-audio.png"><img src="docs/assets/screenshots/20261104/nebula/feature-audio.png" alt="音频路由，nebula主题" width="100%"></a></td>
<td valign="top"><strong>音频路由</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-audio.png"><img src="docs/assets/screenshots/20261104/classic/feature-audio.png" alt="音频路由，classic主题" width="100%"></a></td>
<td valign="top"><strong>音频路由</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-audio.png"><img src="docs/assets/screenshots/20261104/instrument/feature-audio.png" alt="音频路由，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>均衡器</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-eq.png"><img src="docs/assets/screenshots/20261104/nebula/feature-eq.png" alt="均衡器，nebula主题" width="100%"></a></td>
<td valign="top"><strong>均衡器</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-eq.png"><img src="docs/assets/screenshots/20261104/classic/feature-eq.png" alt="均衡器，classic主题" width="100%"></a></td>
<td valign="top"><strong>均衡器</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-eq.png"><img src="docs/assets/screenshots/20261104/instrument/feature-eq.png" alt="均衡器，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

2.1 输出将板载左右声道送往高通分支，USB 低音送往低通分支；两侧截止频率可独立设置，并提供增益、极性和延迟。实际切到耳机时暂时停用 2.1，回到扬声器后恢复保存值，不清空 EQ 或分频设置。两输出的时钟差由队列反馈校正；**完整声学校准尚未完成**，采样峰值限幅也不替代喇叭保护。

通话页保留 HFP 控制入口，真实双向通话另行验收。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>低音炮</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-subwoofer.png"><img src="docs/assets/screenshots/20261104/nebula/feature-subwoofer.png" alt="低音炮，nebula主题" width="100%"></a></td>
<td valign="top"><strong>低音炮</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-subwoofer.png"><img src="docs/assets/screenshots/20261104/classic/feature-subwoofer.png" alt="低音炮，classic主题" width="100%"></a></td>
<td valign="top"><strong>低音炮</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-subwoofer.png"><img src="docs/assets/screenshots/20261104/instrument/feature-subwoofer.png" alt="低音炮，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>通话</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-call.png"><img src="docs/assets/screenshots/20261104/nebula/feature-call.png" alt="通话，nebula主题" width="100%"></a></td>
<td valign="top"><strong>通话</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-call.png"><img src="docs/assets/screenshots/20261104/classic/feature-call.png" alt="通话，classic主题" width="100%"></a></td>
<td valign="top"><strong>通话</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-call.png"><img src="docs/assets/screenshots/20261104/instrument/feature-call.png" alt="通话，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 让灯光跟随设备状态

四个灯光分区可以独立设置，也能跟随瞳色、使用互补色、播放动画或随音乐律动。灯光服务管理开机清零与休眠行为，使显示、声音和灯光能围绕同一次交互工作。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>灯光</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-lighting.png"><img src="docs/assets/screenshots/20261104/nebula/feature-lighting.png" alt="灯光，nebula主题" width="100%"></a></td>
<td valign="top"><strong>灯光</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-lighting.png"><img src="docs/assets/screenshots/20261104/classic/feature-lighting.png" alt="灯光，classic主题" width="100%"></a></td>
<td valign="top"><strong>灯光</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-lighting.png"><img src="docs/assets/screenshots/20261104/instrument/feature-lighting.png" alt="灯光，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 看见人，也把隐私开关交给人

摄影链路由 **openvela 原生采集和 H.264 编码**，面板提供预览、拍照和检测控制。B103 预览画面、B106 真人检测框的位置与移动已获现场确认；下图为关闭预览的设置状态。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>摄影</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-camera.png"><img src="docs/assets/screenshots/20261104/nebula/feature-camera.png" alt="摄影，nebula主题" width="100%"></a></td>
<td valign="top"><strong>摄影</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-camera.png"><img src="docs/assets/screenshots/20261104/classic/feature-camera.png" alt="摄影，classic主题" width="100%"></a></td>
<td valign="top"><strong>摄影</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-camera.png"><img src="docs/assets/screenshots/20261104/instrument/feature-camera.png" alt="摄影，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

身份页把人脸档案与声纹入口放在一起。B108 已处理实际人脸帧并登记一条家人档案；声纹模型已就绪，但真人登记和匹配尚未验收。在场信息与身份能力结合使用，完整认主、活体检测和实体猫眼跟随不能由一次登记结果代替验收。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>身份</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-identity.png"><img src="docs/assets/screenshots/20261104/nebula/feature-identity.png" alt="身份，nebula主题" width="100%"></a></td>
<td valign="top"><strong>身份</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-identity.png"><img src="docs/assets/screenshots/20261104/classic/feature-identity.png" alt="身份，classic主题" width="100%"></a></td>
<td valign="top"><strong>身份</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-identity.png"><img src="docs/assets/screenshots/20261104/instrument/feature-identity.png" alt="身份，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>在场</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-presence.png"><img src="docs/assets/screenshots/20261104/nebula/feature-presence.png" alt="在场，nebula主题" width="100%"></a></td>
<td valign="top"><strong>在场</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-presence.png"><img src="docs/assets/screenshots/20261104/classic/feature-presence.png" alt="在场，classic主题" width="100%"></a></td>
<td valign="top"><strong>在场</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-presence.png"><img src="docs/assets/screenshots/20261104/instrument/feature-presence.png" alt="在场，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

摄像头与麦克风有**各自独立的软件隐私开关**。关闭后由设备端阻止相应采集，设置已验证穿重启保存。远程画面、双向对讲与 Agent 获准后的 MiMo 看图仍需联调。物品学习因模型加载问题已隐藏入口，代码保留待修复。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>隐私</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-privacy.png"><img src="docs/assets/screenshots/20261104/nebula/feature-privacy.png" alt="隐私，nebula主题" width="100%"></a></td>
<td valign="top"><strong>隐私</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-privacy.png"><img src="docs/assets/screenshots/20261104/classic/feature-privacy.png" alt="隐私，classic主题" width="100%"></a></td>
<td valign="top"><strong>隐私</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-privacy.png"><img src="docs/assets/screenshots/20261104/instrument/feature-privacy.png" alt="隐私，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 让计时、提醒和记录留在设备上

短时任务用倒计时，过程计量用秒表，固定或重复提醒用闹钟；日历把日程放回日期中查看。这些状态由设备维护，秒表已验证在没有浏览器推送时继续前进。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>倒计时</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-timer.png"><img src="docs/assets/screenshots/20261104/nebula/feature-timer.png" alt="倒计时，nebula主题" width="100%"></a></td>
<td valign="top"><strong>倒计时</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-timer.png"><img src="docs/assets/screenshots/20261104/classic/feature-timer.png" alt="倒计时，classic主题" width="100%"></a></td>
<td valign="top"><strong>倒计时</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-timer.png"><img src="docs/assets/screenshots/20261104/instrument/feature-timer.png" alt="倒计时，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>秒表</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-stopwatch.png"><img src="docs/assets/screenshots/20261104/nebula/feature-stopwatch.png" alt="秒表，nebula主题" width="100%"></a></td>
<td valign="top"><strong>秒表</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-stopwatch.png"><img src="docs/assets/screenshots/20261104/classic/feature-stopwatch.png" alt="秒表，classic主题" width="100%"></a></td>
<td valign="top"><strong>秒表</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-stopwatch.png"><img src="docs/assets/screenshots/20261104/instrument/feature-stopwatch.png" alt="秒表，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>闹钟</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-alarm.png"><img src="docs/assets/screenshots/20261104/nebula/feature-alarm.png" alt="闹钟，nebula主题" width="100%"></a></td>
<td valign="top"><strong>闹钟</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-alarm.png"><img src="docs/assets/screenshots/20261104/classic/feature-alarm.png" alt="闹钟，classic主题" width="100%"></a></td>
<td valign="top"><strong>闹钟</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-alarm.png"><img src="docs/assets/screenshots/20261104/instrument/feature-alarm.png" alt="闹钟，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>日历</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-calendar.png"><img src="docs/assets/screenshots/20261104/nebula/feature-calendar.png" alt="日历，nebula主题" width="100%"></a></td>
<td valign="top"><strong>日历</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-calendar.png"><img src="docs/assets/screenshots/20261104/classic/feature-calendar.png" alt="日历，classic主题" width="100%"></a></td>
<td valign="top"><strong>日历</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-calendar.png"><img src="docs/assets/screenshots/20261104/instrument/feature-calendar.png" alt="日历，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

睡眠定时用于结束播放，设备休眠用于切换运行状态，两者分别控制。任务页管理待办，记忆页管理持久化记录，方便后续交互继续使用，而不把这些内容绑定在一次浏览器会话里。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>睡眠定时</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-sleep-timer.png"><img src="docs/assets/screenshots/20261104/nebula/feature-sleep-timer.png" alt="睡眠定时，nebula主题" width="100%"></a></td>
<td valign="top"><strong>睡眠定时</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-sleep-timer.png"><img src="docs/assets/screenshots/20261104/classic/feature-sleep-timer.png" alt="睡眠定时，classic主题" width="100%"></a></td>
<td valign="top"><strong>睡眠定时</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-sleep-timer.png"><img src="docs/assets/screenshots/20261104/instrument/feature-sleep-timer.png" alt="睡眠定时，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>休眠</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-sleep.png"><img src="docs/assets/screenshots/20261104/nebula/feature-sleep.png" alt="休眠，nebula主题" width="100%"></a></td>
<td valign="top"><strong>休眠</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-sleep.png"><img src="docs/assets/screenshots/20261104/classic/feature-sleep.png" alt="休眠，classic主题" width="100%"></a></td>
<td valign="top"><strong>休眠</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-sleep.png"><img src="docs/assets/screenshots/20261104/instrument/feature-sleep.png" alt="休眠，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>任务</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-task.png"><img src="docs/assets/screenshots/20261104/nebula/feature-task.png" alt="任务，nebula主题" width="100%"></a></td>
<td valign="top"><strong>任务</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-task.png"><img src="docs/assets/screenshots/20261104/classic/feature-task.png" alt="任务，classic主题" width="100%"></a></td>
<td valign="top"><strong>任务</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-task.png"><img src="docs/assets/screenshots/20261104/instrument/feature-task.png" alt="任务，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>记忆</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-memory.png"><img src="docs/assets/screenshots/20261104/nebula/feature-memory.png" alt="记忆，nebula主题" width="100%"></a></td>
<td valign="top"><strong>记忆</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-memory.png"><img src="docs/assets/screenshots/20261104/classic/feature-memory.png" alt="记忆，classic主题" width="100%"></a></td>
<td valign="top"><strong>记忆</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-memory.png"><img src="docs/assets/screenshots/20261104/instrument/feature-memory.png" alt="记忆，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

天气服务支持当前、小时与多日预报，并保留来源、缓存和过期提示；Open-Meteo 数据属于模式估计。简报将时间、天气等服务内容组织在一起，陪伴页管理主动陪伴设置。这里呈现的是服务与配置，不把它们扩写成情绪识别或健康诊断。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>天气</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-weather.png"><img src="docs/assets/screenshots/20261104/nebula/feature-weather.png" alt="天气，nebula主题" width="100%"></a></td>
<td valign="top"><strong>天气</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-weather.png"><img src="docs/assets/screenshots/20261104/classic/feature-weather.png" alt="天气，classic主题" width="100%"></a></td>
<td valign="top"><strong>天气</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-weather.png"><img src="docs/assets/screenshots/20261104/instrument/feature-weather.png" alt="天气，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>简报</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-briefing.png"><img src="docs/assets/screenshots/20261104/nebula/feature-briefing.png" alt="简报，nebula主题" width="100%"></a></td>
<td valign="top"><strong>简报</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-briefing.png"><img src="docs/assets/screenshots/20261104/classic/feature-briefing.png" alt="简报，classic主题" width="100%"></a></td>
<td valign="top"><strong>简报</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-briefing.png"><img src="docs/assets/screenshots/20261104/instrument/feature-briefing.png" alt="简报，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>陪伴</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-companion.png"><img src="docs/assets/screenshots/20261104/nebula/feature-companion.png" alt="陪伴，nebula主题" width="100%"></a></td>
<td valign="top"><strong>陪伴</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-companion.png"><img src="docs/assets/screenshots/20261104/classic/feature-companion.png" alt="陪伴，classic主题" width="100%"></a></td>
<td valign="top"><strong>陪伴</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-companion.png"><img src="docs/assets/screenshots/20261104/instrument/feature-companion.png" alt="陪伴，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 让 Agent 给设备增加工具

扩展不只意味着预装更多功能。主人开启“自动生成与安装”后，可以提出“做一个本地记账本”，再继续要求分类筛选或修复输入校验。Agent 读取契约与已有版本、生成插件、由设备签名安装，并通过实际 UI 事件检查结果；失败时恢复旧代码、私有数据与授权。

插件安装、授权与启动是独立步骤。下列已安装实例在采集时均保持停止；每组先列详情，再列对应权限，便于对照一个工具能访问什么。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>扩展</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugins.png"><img src="docs/assets/screenshots/20261104/nebula/plugins.png" alt="扩展，nebula主题" width="100%"></a></td>
<td valign="top"><strong>扩展</strong><br><a href="docs/assets/screenshots/20261104/classic/plugins.png"><img src="docs/assets/screenshots/20261104/classic/plugins.png" alt="扩展，classic主题" width="100%"></a></td>
<td valign="top"><strong>扩展</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugins.png"><img src="docs/assets/screenshots/20261104/instrument/plugins.png" alt="扩展，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

#### 从一个计数器开始

陪伴计数器演示插件状态与界面事件；上传计数器用于核对从打包、上传到设备安装的入口。同一个工具的详情和权限分开管理，安装成功不会替用户自动授权。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>插件 companion-counter</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-companion-counter.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-companion-counter.png" alt="插件 companion-counter，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 companion-counter</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-companion-counter.png"><img src="docs/assets/screenshots/20261104/classic/plugin-companion-counter.png" alt="插件 companion-counter，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 companion-counter</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-companion-counter.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-companion-counter.png" alt="插件 companion-counter，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 companion-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-companion-counter-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-companion-counter-permissions.png" alt="插件 companion-counter 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 companion-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-companion-counter-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-companion-counter-permissions.png" alt="插件 companion-counter 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 companion-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-companion-counter-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-companion-counter-permissions.png" alt="插件 companion-counter 权限，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 board-upload-counter</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-board-upload-counter.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-board-upload-counter.png" alt="插件 board-upload-counter，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 board-upload-counter</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-board-upload-counter.png"><img src="docs/assets/screenshots/20261104/classic/plugin-board-upload-counter.png" alt="插件 board-upload-counter，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 board-upload-counter</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-board-upload-counter.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-board-upload-counter.png" alt="插件 board-upload-counter，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 board-upload-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-board-upload-counter-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-board-upload-counter-permissions.png" alt="插件 board-upload-counter 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 board-upload-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-board-upload-counter-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-board-upload-counter-permissions.png" alt="插件 board-upload-counter 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 board-upload-counter 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-board-upload-counter-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-board-upload-counter-permissions.png" alt="插件 board-upload-counter 权限，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

#### 接入硬件与界面资源

硬件面板演示通过受控接口读取和操作设备，图标面板演示组件与图标资源。插件声明的能力仍需主人授予；停止或撤权后，不能继续凭旧页面调用设备。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>插件 hardware-panel</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-hardware-panel.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-hardware-panel.png" alt="插件 hardware-panel，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 hardware-panel</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-hardware-panel.png"><img src="docs/assets/screenshots/20261104/classic/plugin-hardware-panel.png" alt="插件 hardware-panel，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 hardware-panel</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-hardware-panel.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-hardware-panel.png" alt="插件 hardware-panel，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 hardware-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-hardware-panel-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-hardware-panel-permissions.png" alt="插件 hardware-panel 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 hardware-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-hardware-panel-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-hardware-panel-permissions.png" alt="插件 hardware-panel 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 hardware-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-hardware-panel-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-hardware-panel-permissions.png" alt="插件 hardware-panel 权限，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 iconfont-panel</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-iconfont-panel.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-iconfont-panel.png" alt="插件 iconfont-panel，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 iconfont-panel</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-iconfont-panel.png"><img src="docs/assets/screenshots/20261104/classic/plugin-iconfont-panel.png" alt="插件 iconfont-panel，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 iconfont-panel</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-iconfont-panel.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-iconfont-panel.png" alt="插件 iconfont-panel，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 iconfont-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-iconfont-panel-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-iconfont-panel-permissions.png" alt="插件 iconfont-panel 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 iconfont-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-iconfont-panel-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-iconfont-panel-permissions.png" alt="插件 iconfont-panel 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 iconfont-panel 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-iconfont-panel-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-iconfont-panel-permissions.png" alt="插件 iconfont-panel 权限，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

#### 三个在真机上生成过的工具

**中文记账本**支持金额校验、分类、备注、合计、筛选和删除；数据在更新与重启后保留。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>插件 agent-ledger-cn</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-ledger-cn.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-ledger-cn.png" alt="插件 agent-ledger-cn，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-ledger-cn</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-ledger-cn.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-ledger-cn.png" alt="插件 agent-ledger-cn，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-ledger-cn</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-ledger-cn.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-ledger-cn.png" alt="插件 agent-ledger-cn，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 agent-ledger-cn 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-ledger-cn-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-ledger-cn-permissions.png" alt="插件 agent-ledger-cn 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-ledger-cn 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-ledger-cn-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-ledger-cn-permissions.png" alt="插件 agent-ledger-cn 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-ledger-cn 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-ledger-cn-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-ledger-cn-permissions.png" alt="插件 agent-ledger-cn 权限，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

**传输计算器**区分 GB/GiB、MB/s/Mbps，并考虑效率、预计时长与历史。它也验证了运行时 Skill 的作用：设备导入并启用 `transfer-planning` 后，模型读取其知识来生成和修复工具。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>插件 agent-transfer-planner</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-transfer-planner.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-transfer-planner.png" alt="插件 agent-transfer-planner，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-transfer-planner</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-transfer-planner.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-transfer-planner.png" alt="插件 agent-transfer-planner，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-transfer-planner</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-transfer-planner.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-transfer-planner.png" alt="插件 agent-transfer-planner，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 agent-transfer-planner 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-transfer-planner-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-transfer-planner-permissions.png" alt="插件 agent-transfer-planner 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-transfer-planner 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-transfer-planner-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-transfer-planner-permissions.png" alt="插件 agent-transfer-planner 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-transfer-planner 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-transfer-planner-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-transfer-planner-permissions.png" alt="插件 agent-transfer-planner 权限，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

**设备体检**读取系统、音频、蓝牙和 USB 状态，通过真实刷新事件获取新值，并标明单位。上述运行结果见[产品技术说明](docs/product/README.md#agent-在设备上创建新功能)，本组截图展示安装后的管理状态。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>插件 agent-device-health</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-device-health.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-device-health.png" alt="插件 agent-device-health，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-device-health</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-device-health.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-device-health.png" alt="插件 agent-device-health，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-device-health</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-device-health.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-device-health.png" alt="插件 agent-device-health，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>插件 agent-device-health 权限</strong><br><a href="docs/assets/screenshots/20261104/nebula/plugin-agent-device-health-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/plugin-agent-device-health-permissions.png" alt="插件 agent-device-health 权限，nebula主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-device-health 权限</strong><br><a href="docs/assets/screenshots/20261104/classic/plugin-agent-device-health-permissions.png"><img src="docs/assets/screenshots/20261104/classic/plugin-agent-device-health-permissions.png" alt="插件 agent-device-health 权限，classic主题" width="100%"></a></td>
<td valign="top"><strong>插件 agent-device-health 权限</strong><br><a href="docs/assets/screenshots/20261104/instrument/plugin-agent-device-health-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/plugin-agent-device-health-permissions.png" alt="插件 agent-device-health 权限，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

扩展也面向开发者：JavaScript / TypeScript 使用 QuickJS，Wasm 使用 WAMR；SDK 提供打包、签名、能力接口和示例。运行时 Skill 提供知识，不增加权限；双向 MCP 则让设备调用外部工具，或让外部 Agent 在限定权限内访问设备。具体接入方式见[扩展指南](docs/product/EXTENSIONS.md)。

### 从本地连接到远程访问

设备密码保护本地面板，配对入口负责建立访问关系；Cloud 账号用于认领和远程连接，两套身份各有用途。账号行使用本地模拟器补拍的空表单，没有登录云账号或展示个人数据。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>设备登录</strong><br><a href="docs/assets/screenshots/20261104/nebula/login.png"><img src="docs/assets/screenshots/20261104/nebula/login.png" alt="设备登录，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备登录</strong><br><a href="docs/assets/screenshots/20261104/classic/login.png"><img src="docs/assets/screenshots/20261104/classic/login.png" alt="设备登录，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备登录</strong><br><a href="docs/assets/screenshots/20261104/instrument/login.png"><img src="docs/assets/screenshots/20261104/instrument/login.png" alt="设备登录，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>配对</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-pairing.png"><img src="docs/assets/screenshots/20261104/nebula/feature-pairing.png" alt="配对，nebula主题" width="100%"></a></td>
<td valign="top"><strong>配对</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-pairing.png"><img src="docs/assets/screenshots/20261104/classic/feature-pairing.png" alt="配对，classic主题" width="100%"></a></td>
<td valign="top"><strong>配对</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-pairing.png"><img src="docs/assets/screenshots/20261104/instrument/feature-pairing.png" alt="配对，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>Cloud 账号 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/nebula/account.png"><img src="docs/assets/screenshots/20261105-sim/nebula/account.png" alt="Cloud 账号 · 模拟器，nebula主题" width="100%"></a></td>
<td valign="top"><strong>Cloud 账号 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/classic/account.png"><img src="docs/assets/screenshots/20261105-sim/classic/account.png" alt="Cloud 账号 · 模拟器，classic主题" width="100%"></a></td>
<td valign="top"><strong>Cloud 账号 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/instrument/account.png"><img src="docs/assets/screenshots/20261105-sim/instrument/account.png" alt="Cloud 账号 · 模拟器，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

设备访问设置与 Cloud 中继分别配置。设备主动连接中继，账号认领与设备密码分别检查；本地面板和远控最终都接受设备端权限检查。owner、family、guest 的能力不同，角色变更和撤销已验证使旧会话失效。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>设备设置：访问密码</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-access.png"><img src="docs/assets/screenshots/20261104/nebula/device-access.png" alt="设备设置：访问密码，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：访问密码</strong><br><a href="docs/assets/screenshots/20261104/classic/device-access.png"><img src="docs/assets/screenshots/20261104/classic/device-access.png" alt="设备设置：访问密码，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：访问密码</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-access.png"><img src="docs/assets/screenshots/20261104/instrument/device-access.png" alt="设备设置：访问密码，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：云中继</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-cloud.png"><img src="docs/assets/screenshots/20261104/nebula/device-cloud.png" alt="设备设置：云中继，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：云中继</strong><br><a href="docs/assets/screenshots/20261104/classic/device-cloud.png"><img src="docs/assets/screenshots/20261104/classic/device-cloud.png" alt="设备设置：云中继，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：云中继</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-cloud.png"><img src="docs/assets/screenshots/20261104/instrument/device-cloud.png" alt="设备设置：云中继，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：权限总览</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-permissions.png"><img src="docs/assets/screenshots/20261104/nebula/device-permissions.png" alt="设备设置：权限总览，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：权限总览</strong><br><a href="docs/assets/screenshots/20261104/classic/device-permissions.png"><img src="docs/assets/screenshots/20261104/classic/device-permissions.png" alt="设备设置：权限总览，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：权限总览</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-permissions.png"><img src="docs/assets/screenshots/20261104/instrument/device-permissions.png" alt="设备设置：权限总览，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 把日常设置与故障排查分开

工作区设置把设备维护入口集中起来；设备目录与概览用于确认当前维护的对象。外观偏好留在客户端，网络、模型、权限和更新则属于设备。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>工作区设置</strong><br><a href="docs/assets/screenshots/20261104/nebula/settings.png"><img src="docs/assets/screenshots/20261104/nebula/settings.png" alt="工作区设置，nebula主题" width="100%"></a></td>
<td valign="top"><strong>工作区设置</strong><br><a href="docs/assets/screenshots/20261104/classic/settings.png"><img src="docs/assets/screenshots/20261104/classic/settings.png" alt="工作区设置，classic主题" width="100%"></a></td>
<td valign="top"><strong>工作区设置</strong><br><a href="docs/assets/screenshots/20261104/instrument/settings.png"><img src="docs/assets/screenshots/20261104/instrument/settings.png" alt="工作区设置，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置目录</strong><br><a href="docs/assets/screenshots/20261104/nebula/device.png"><img src="docs/assets/screenshots/20261104/nebula/device.png" alt="设备设置目录，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置目录</strong><br><a href="docs/assets/screenshots/20261104/classic/device.png"><img src="docs/assets/screenshots/20261104/classic/device.png" alt="设备设置目录，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置目录</strong><br><a href="docs/assets/screenshots/20261104/instrument/device.png"><img src="docs/assets/screenshots/20261104/instrument/device.png" alt="设备设置目录，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：概览</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-overview.png"><img src="docs/assets/screenshots/20261104/nebula/device-overview.png" alt="设备设置：概览，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：概览</strong><br><a href="docs/assets/screenshots/20261104/classic/device-overview.png"><img src="docs/assets/screenshots/20261104/classic/device-overview.png" alt="设备设置：概览，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：概览</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-overview.png"><img src="docs/assets/screenshots/20261104/instrument/device-overview.png" alt="设备设置：概览，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

日常网络页便于查看连接状态，设备网络设置用于配置 Wi-Fi。设备列表显示已注册外设，系统页显示 CPU、温度和资源占用；排查时可以把网络断连、服务状态与浏览器显示问题区分开。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>网络</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-network.png"><img src="docs/assets/screenshots/20261104/nebula/feature-network.png" alt="网络，nebula主题" width="100%"></a></td>
<td valign="top"><strong>网络</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-network.png"><img src="docs/assets/screenshots/20261104/classic/feature-network.png" alt="网络，classic主题" width="100%"></a></td>
<td valign="top"><strong>网络</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-network.png"><img src="docs/assets/screenshots/20261104/instrument/feature-network.png" alt="网络，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：网络</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-network.png"><img src="docs/assets/screenshots/20261104/nebula/device-network.png" alt="设备设置：网络，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：网络</strong><br><a href="docs/assets/screenshots/20261104/classic/device-network.png"><img src="docs/assets/screenshots/20261104/classic/device-network.png" alt="设备设置：网络，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：网络</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-network.png"><img src="docs/assets/screenshots/20261104/instrument/device-network.png" alt="设备设置：网络，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-devices.png"><img src="docs/assets/screenshots/20261104/nebula/feature-devices.png" alt="设备，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-devices.png"><img src="docs/assets/screenshots/20261104/classic/feature-devices.png" alt="设备，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-devices.png"><img src="docs/assets/screenshots/20261104/instrument/feature-devices.png" alt="设备，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>系统</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-system.png"><img src="docs/assets/screenshots/20261104/nebula/feature-system.png" alt="系统，nebula主题" width="100%"></a></td>
<td valign="top"><strong>系统</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-system.png"><img src="docs/assets/screenshots/20261104/classic/feature-system.png" alt="系统，classic主题" width="100%"></a></td>
<td valign="top"><strong>系统</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-system.png"><img src="docs/assets/screenshots/20261104/instrument/feature-system.png" alt="系统，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

存储页查看空间与挂载情况，USB 文件共享提供认证后的浏览、上传与下载，默认关闭、只读。该共享服务的完整实物盘验收尚待补齐。模型独立于固件交付，模型页检查资源与计算域状态，更新固件时无需重复打包全部权重。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>设备设置：存储</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-storage.png"><img src="docs/assets/screenshots/20261104/nebula/device-storage.png" alt="设备设置：存储，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：存储</strong><br><a href="docs/assets/screenshots/20261104/classic/device-storage.png"><img src="docs/assets/screenshots/20261104/classic/device-storage.png" alt="设备设置：存储，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：存储</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-storage.png"><img src="docs/assets/screenshots/20261104/instrument/device-storage.png" alt="设备设置：存储，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>USB 文件共享</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-nas.png"><img src="docs/assets/screenshots/20261104/nebula/feature-nas.png" alt="USB 文件共享，nebula主题" width="100%"></a></td>
<td valign="top"><strong>USB 文件共享</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-nas.png"><img src="docs/assets/screenshots/20261104/classic/feature-nas.png" alt="USB 文件共享，classic主题" width="100%"></a></td>
<td valign="top"><strong>USB 文件共享</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-nas.png"><img src="docs/assets/screenshots/20261104/instrument/feature-nas.png" alt="USB 文件共享，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：模型</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-models.png"><img src="docs/assets/screenshots/20261104/nebula/device-models.png" alt="设备设置：模型，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：模型</strong><br><a href="docs/assets/screenshots/20261104/classic/device-models.png"><img src="docs/assets/screenshots/20261104/classic/device-models.png" alt="设备设置：模型，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：模型</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-models.png"><img src="docs/assets/screenshots/20261104/instrument/device-models.png" alt="设备设置：模型，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

更新页读取 **普通 openvela 与 AMP 两个启动域的 A/B 四槽状态**，显示运行/启动标记、可启动性、版本和镜像大小。常规更新写入非运行槽并回读校验；展开高级选项后，还能看到 **N-Boot、trust、bootctrl、nuttx_a/b、amp_a/b、config、data** 的维护目标与写入条件。运行槽和 `data` 禁止从面板覆盖，挂载分区与引导程序保留风险确认。

下图已展开全部高级目标，使用按固件目标定义配置的模拟器演示数据。日志用于定位错误，关于页用于核对版本；当前实机保留 A21 恢复槽。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>更新 · 高级展开 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/nebula/device-update-advanced.png"><img src="docs/assets/screenshots/20261105-sim/nebula/device-update-advanced.png" alt="高级更新展开，nebula主题，模拟器" width="100%"></a></td>
<td valign="top"><strong>更新 · 高级展开 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/classic/device-update-advanced.png"><img src="docs/assets/screenshots/20261105-sim/classic/device-update-advanced.png" alt="高级更新展开，classic主题，模拟器" width="100%"></a></td>
<td valign="top"><strong>更新 · 高级展开 · 模拟器</strong><br><a href="docs/assets/screenshots/20261105-sim/instrument/device-update-advanced.png"><img src="docs/assets/screenshots/20261105-sim/instrument/device-update-advanced.png" alt="高级更新展开，instrument主题，模拟器" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：日志</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-logs.png"><img src="docs/assets/screenshots/20261104/nebula/device-logs.png" alt="设备设置：日志，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：日志</strong><br><a href="docs/assets/screenshots/20261104/classic/device-logs.png"><img src="docs/assets/screenshots/20261104/classic/device-logs.png" alt="设备设置：日志，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：日志</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-logs.png"><img src="docs/assets/screenshots/20261104/instrument/device-logs.png" alt="设备设置：日志，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>设备设置：关于</strong><br><a href="docs/assets/screenshots/20261104/nebula/device-about.png"><img src="docs/assets/screenshots/20261104/nebula/device-about.png" alt="设备设置：关于，nebula主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：关于</strong><br><a href="docs/assets/screenshots/20261104/classic/device-about.png"><img src="docs/assets/screenshots/20261104/classic/device-about.png" alt="设备设置：关于，classic主题" width="100%"></a></td>
<td valign="top"><strong>设备设置：关于</strong><br><a href="docs/assets/screenshots/20261104/instrument/device-about.png"><img src="docs/assets/screenshots/20261104/instrument/device-about.png" alt="设备设置：关于，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

### 同一套功能，适应不同屏幕

客户端可以切换星云、经典、仪器三种皮肤，以及深浅色、配色和布局。这些选择保存在当前浏览器；无论用哪种外观，操作的仍是同一台设备。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>客户端设置</strong><br><a href="docs/assets/screenshots/20261104/nebula/client-settings.png"><img src="docs/assets/screenshots/20261104/nebula/client-settings.png" alt="客户端设置，nebula主题" width="100%"></a></td>
<td valign="top"><strong>客户端设置</strong><br><a href="docs/assets/screenshots/20261104/classic/client-settings.png"><img src="docs/assets/screenshots/20261104/classic/client-settings.png" alt="客户端设置，classic主题" width="100%"></a></td>
<td valign="top"><strong>客户端设置</strong><br><a href="docs/assets/screenshots/20261104/instrument/client-settings.png"><img src="docs/assets/screenshots/20261104/instrument/client-settings.png" alt="客户端设置，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

浅色模式保留原有主页、功能入口和摄影控制；摄影能力仍受设备隐私开关约束。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>主页 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/nebula/home-light.png"><img src="docs/assets/screenshots/20261104/nebula/home-light.png" alt="主页 · 浅色，nebula主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/classic/home-light.png"><img src="docs/assets/screenshots/20261104/classic/home-light.png" alt="主页 · 浅色，classic主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/instrument/home-light.png"><img src="docs/assets/screenshots/20261104/instrument/home-light.png" alt="主页 · 浅色，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>功能 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/nebula/features-light.png"><img src="docs/assets/screenshots/20261104/nebula/features-light.png" alt="功能 · 浅色，nebula主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/classic/features-light.png"><img src="docs/assets/screenshots/20261104/classic/features-light.png" alt="功能 · 浅色，classic主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/instrument/features-light.png"><img src="docs/assets/screenshots/20261104/instrument/features-light.png" alt="功能 · 浅色，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>摄影 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/nebula/feature-camera-light.png"><img src="docs/assets/screenshots/20261104/nebula/feature-camera-light.png" alt="摄影 · 浅色，nebula主题" width="100%"></a></td>
<td valign="top"><strong>摄影 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/classic/feature-camera-light.png"><img src="docs/assets/screenshots/20261104/classic/feature-camera-light.png" alt="摄影 · 浅色，classic主题" width="100%"></a></td>
<td valign="top"><strong>摄影 · 浅色</strong><br><a href="docs/assets/screenshots/20261104/instrument/feature-camera-light.png"><img src="docs/assets/screenshots/20261104/instrument/feature-camera-light.png" alt="摄影 · 浅色，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

手机将状态与操作纵向组织，平板保留更宽的操作区域。下面分别对照主页与功能目录，完整长图可点击放大。

<table>
<thead><tr><th width="33%">星云 Nebula</th><th width="33%">经典 Classic</th><th width="33%">仪器 Instrument</th></tr></thead>
<tbody>
<tr>
<td valign="top"><strong>主页 · 手机</strong><br><a href="docs/assets/screenshots/20261104/nebula/home-phone.png"><img src="docs/assets/screenshots/20261104/nebula/home-phone.png" alt="主页 · 手机，nebula主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 手机</strong><br><a href="docs/assets/screenshots/20261104/classic/home-phone.png"><img src="docs/assets/screenshots/20261104/classic/home-phone.png" alt="主页 · 手机，classic主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 手机</strong><br><a href="docs/assets/screenshots/20261104/instrument/home-phone.png"><img src="docs/assets/screenshots/20261104/instrument/home-phone.png" alt="主页 · 手机，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>功能 · 手机</strong><br><a href="docs/assets/screenshots/20261104/nebula/features-phone.png"><img src="docs/assets/screenshots/20261104/nebula/features-phone.png" alt="功能 · 手机，nebula主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 手机</strong><br><a href="docs/assets/screenshots/20261104/classic/features-phone.png"><img src="docs/assets/screenshots/20261104/classic/features-phone.png" alt="功能 · 手机，classic主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 手机</strong><br><a href="docs/assets/screenshots/20261104/instrument/features-phone.png"><img src="docs/assets/screenshots/20261104/instrument/features-phone.png" alt="功能 · 手机，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>主页 · 平板</strong><br><a href="docs/assets/screenshots/20261104/nebula/home-tablet.png"><img src="docs/assets/screenshots/20261104/nebula/home-tablet.png" alt="主页 · 平板，nebula主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 平板</strong><br><a href="docs/assets/screenshots/20261104/classic/home-tablet.png"><img src="docs/assets/screenshots/20261104/classic/home-tablet.png" alt="主页 · 平板，classic主题" width="100%"></a></td>
<td valign="top"><strong>主页 · 平板</strong><br><a href="docs/assets/screenshots/20261104/instrument/home-tablet.png"><img src="docs/assets/screenshots/20261104/instrument/home-tablet.png" alt="主页 · 平板，instrument主题" width="100%"></a></td>
</tr>
<tr>
<td valign="top"><strong>功能 · 平板</strong><br><a href="docs/assets/screenshots/20261104/nebula/features-tablet.png"><img src="docs/assets/screenshots/20261104/nebula/features-tablet.png" alt="功能 · 平板，nebula主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 平板</strong><br><a href="docs/assets/screenshots/20261104/classic/features-tablet.png"><img src="docs/assets/screenshots/20261104/classic/features-tablet.png" alt="功能 · 平板，classic主题" width="100%"></a></td>
<td valign="top"><strong>功能 · 平板</strong><br><a href="docs/assets/screenshots/20261104/instrument/features-tablet.png"><img src="docs/assets/screenshots/20261104/instrument/features-tablet.png" alt="功能 · 平板，instrument主题" width="100%"></a></td>
</tr>
</tbody>
</table>

## 获取与运行

在 Linux 构建环境使用完整 manifest；只克隆本仓不足以构建 openvela：

```sh
mkdir nyabula-openvela && cd nyabula-openvela
repo init -u https://github.com/open-vela/contest2026_062_PharosTech \
  -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j4
```

按用途选择基础 BSP、独立平台功能、普通产品或 **4+4 AMP 产品**配置，再按[构建运行指南](docs/GETTING_STARTED.md)准备字体、无线固件、模型和启动器。15 套配置的历史 CMake / Make 检查记录及各项板测范围见[版本与验证](docs/validation/README.md)。

| 查找内容 | 入口 |
|---|---|
| 驱动、板级与配置 | [平台适配](docs/platform/README.md) |
| 产品机制与交互 | [产品说明](docs/product/README.md) · [系统架构](docs/product/ARCHITECTURE.md) |
| 开发插件、Skill 或 MCP | [扩展指南](docs/product/EXTENSIONS.md) · [插件 SDK](tools/nyabula_plugin/README.md) |
| Go Cloud 与协议 | [Cloud 服务](tools/nyabula_cloud/README.md) · [NyaLink 协议](docs/protocol/nyalink.md) |
| 已完成与待验收项 | [能力清单](docs/CAPABILITIES.md) · [验证记录](docs/validation/README.md) |

## AI 开发与团队交付

Pharos Tech：**陈尚宏**负责队长、硬件适配与架构设计；**池泓翔**负责硬件适配与外设配件研发。具体代码与评审贡献保留 Git / PR 署名。

开发过程沉淀为 **[24 份工程 Skill](skills/README.md)**，覆盖移植、驱动、AMP、模型、插件、MCP 与交付；设备运行时的 [transfer-planning Skill](tools/nyabula_plugin/skills/transfer-planning/SKILL.md)另计。[AI Coding 日志](logs/README.md)保留开发来源、工具事件和继承关系。

N-Boot 是 U-Boot 下游；RKNN / RKLLM、无线固件、模型、字体和其他依赖遵循各自来源与许可。团队自有代码按文件中的 Apache-2.0 标记提供，外部资源与镜像交付范围见[版本与验证](docs/validation/README.md#源码与外部资源)。
