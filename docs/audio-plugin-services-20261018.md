# 音频服务：DSP、USB 2.1 与手机蓝牙

## 从 WebUI 使用

连接启用本候选固件的设备，在“功能”中打开：

- **均衡器**：16段可编辑频率、Q、增益，支持钟形/低架/高架、单段开关；拖动曲线圆点或输入数值，再点击“应用并保存”。丰富、平衡、温和三个预设点击即应用；手动调节默认实时生效，满意后保存，撤销回到已保存配置。“我的预设”可命名保存、应用、覆盖和删除，最多8份。独立“启用均衡器”开关保留所有参数；“重置均衡器”恢复16段默认频率/Q/零增益，不改主增益、系统音量或已保存预设。
- **低音炮**：停止所有普通音频后启用 USB 输出，设定 LR4 分频点、三路增益、极性和延迟。
- **蓝牙音频**：打开 120 秒配对窗口，用手机连接 Nyabula，并在页面确认配对请求；有六位数字时必须核对两端一致。配对窗口默认关闭。
- **通话**：显示 HFP 实际来电状态，接听、拒接和挂断；显示 SCO 收发计数。

蓝牙音乐、TTS 和音乐页文件播放自动经过公共 DSP；通话保留原有 8/16 kHz
低延迟路径。44.1 kHz 等输入经过有抗混叠滤波的重采样转为 48 kHz，不改变语速。
音乐页仍支持原有标准 PCM16 mono/stereo WAV（8–192 kHz）及暂停、恢复、停止。
EQ 页下方折叠的“测试文件播放”保留原有 48 kHz 双声道测试入口。

低音炮未连接前保持关闭，左右输出全频。B54 已保留用户最新的四段频率/Q/增益并扩展12个平直频段，不覆盖成预设。
三个预设按对数频率能量估算电平补偿，未做音乐LUFS匹配或箱体声学校准。
Windows 模拟器只保存控制配置，不产生真实音频，页面明确标明模拟状态。

## 当前实现与验收边界

| 项目 | 已实现 | 证据与剩余验收 |
|---|---|---|
| 插件管理 | 中文显示名、列表开关、压缩包安装弹窗；签名校验与权限分离 | B37/B45真机完成签名包上传、中文名、列表启停；PNG/WOFF2图标与运行中硬件撤权验证通过 |
| 公共 DSP | 蓝牙/TTS/文件共用，16段参数 EQ 与架式滤波器，主增益、LR4、声道校准、峰值保护 | B53 文件暂停/恢复/停止通过；B54第16段实时修改、撤销、三个预设及旧设置迁移通过，TTS 用户确认“声音连续正常”；A2DP 44.1→48 kHz 连续处理、热调参 error=0 |
| 运行中调参 | 新实例预热最大延迟与滤波历史，再以一个处理块交叉淡化 | 宿主实际公共 DSP 出口及 ASan/UBSan 通过；B53 蓝牙播放中更新 EQ 成功 |
| USB 2.1 | ES8388 左右输出 + USB 低频双单声道输出，独立设备队列；低频队列反馈线性重采样 | ±1500 ppm模拟漂移回归通过；B52双设备数字流跑完294912帧、lastError=0，记录1次USB队列欠载；用户低音扬声器尚未接入，长期漂移、相位、延迟及听感待测 |
| 蓝牙 | 原生 ZBlue A2DP Sink + AVRCP + HFP HF，SBC/mSBC、H2 成帧、SCO 收发、门控、配对持久化 | B45 WinRT数字核对配对成功；A2DP实传5秒、250包；旧欠载计数因队列记账错误失效，不能作为连续播放证据。B50已修复；HFP连接及mSBC上行已观测，双向与真实手机通话待验收 |

产品仍为 **4 A53 openvela + 4 A72 Linux**。音频设备和蓝牙由 openvela 的现有
SAI/ES8388、USB Audio、SV6621 HCI 驱动承载，不改 Linux 外设所有权。
已获用户授权刷板，始终保留 A21 成功恢复镜像，只迭代 B 槽。没有 push 或更改远端 PR。
逐次镜像、失败与恢复证据以项目根目录《实测日志.md》为准。

## DSP 接口与参数

`dsp.status` 返回设备节点、配置、运行状态、当前 `source`（bluetooth/tts/file/test）、处理帧、输出峰值、USB 欠载和队列校正量。
`dsp.config` 默认在写入产品存储成功后生效。`persist:false` 仅实时应用内存配置，不写存储；`restore:true` 回到上次保存配置。状态的 `settingsSaved` 区分已保存与实时预览。下例保留旧四段接口写法：

```json
{
  "masterDb": -12,
  "crossoverHz": 120,
  "bassEnabled": false,
  "bypass": false,
  "eqBands": [
    {"frequencyHz":80,"q":1,"gainDb":0,"type":"peak","enabled":true},
    {"frequencyHz":300,"q":1,"gainDb":0,"type":"peak","enabled":true},
    {"frequencyHz":1200,"q":1,"gainDb":0,"type":"peak","enabled":true},
    {"frequencyHz":6000,"q":1,"gainDb":0,"type":"peak","enabled":true}
  ],
  "outputGainDb": [0, 0, 0],
  "delayMs": [0, 0, 0],
  "invert": [false, false, false]
}
```

- 主增益 -30..0 dB；产品分频范围 40..500 Hz。
- `satelliteHighpassHz` 控制左右板载高通，`bassLowpassHz` 控制低音低通，
  两路均采用LR4。2.1已启用时，可用下列请求实时试听250–500Hz重叠区，
  不改变EQ、增益、极性、延时或已保存设置：

  ```json
  {"satelliteHighpassHz":250,"bassLowpassHz":500,"persist":false}
  ```

  两个截止点独立，不自动补偿重叠区的声压或相位；需要结合实际扬声器和摆位调音。
  `crossoverHz` 旧写法仍同时设置两路；同一请求包含独立字段时，各自独立字段优先。
  仅更新一个独立字段会保留另一路。读取时旧字段等于板载高通值；旧存储仅有一个
  分频点时两路都继承它。EQ预设不保存或覆盖这两个输出校准值。
  新面板从返回字段识别独立分频能力，连接旧固件时仍显示原单点控制。
  若保存了超过300Hz的板载高通，回退旧上限300Hz固件前需恢复兼容的已保存设置。
- `eqBands16` 是新的完整16段输入EQ；`eqBands` 保留旧四段兼容接口，映射至16段索引2/5/8/12（从0开始）。每段：20..18000 Hz、Q 0.2..10、增益 −12..12 dB；类型 `peak` / `lowShelf` / `highShelf`。架式 Q≈0.707 平顺，更高 Q 会共振。
- 旧 `eqDb[4]` 继续接受并返回，只更新对应段增益、不清除频率/Q/类型；同一请求的优先级为 `eqBands16` > `eqBands` > `eqDb`。旧存储迁移时沿用80/300/1200/6000 Hz与Q=1。
- 曲线为16段 EQ 的理论响应，不含主增益、分频或限幅；余量建议按 EQ 最大提升与输出增益估算，不是实测声压。
- 三路顺序为左、右、低频；输出增益 -24..6 dB。
- `bassEnabled=false` 使用全频立体声 EQ，不会把低频从左右声道切掉。
- `eqEnabled=false` 只关闭输入EQ，保留主增益、输出校准和分频。缺省为true，旧设置兼容。`resetEq:true` 重置输入EQ并启用，不改主增益或输出校准。
- `bypass=true` 是干声左右 + 静音低频，保留主增益与限幅，跳过 EQ/分频/输出延迟等。
- 播放中可调音效，输出模式 `bassEnabled` 必须停播后切换。
- `dsp.play {"track":"example.wav"}` 仅接受音乐目录内的文件名；请求交给常驻产品任务启动，
  调用者退出不会结束播放。`dsp.stop {}` 仅停止此测试播放；普通音源使用各自的停止控制。
- USB 缺失/输出失败会结束播放并返回错误，不静默丢弃低频或切换另一设备。
- USB 队列目标 512 帧，校正限制 ±5000 ppm；此数值是控制器指令，不是实测晶振偏差或相位对齐证据。分频本身固定 48 kHz。
- 家人可读取和控制 DSP；访客无权操作。蓝牙控制仅主人，家人可读取 `bt.status`。

丰富/平衡/温和预设的主增益分别为-8.5/-7/-6.5 dB；点击预设会同时应用对应主增益。DSP首次默认主增益 -12 dB。数字限幅不是扬声器位移或温度保护；首次接功放保持低音量，
按单元和箱体校准分频、增益与延迟。

## 保存自定义调音

先调好声音，在“为当前调音命名”输入名称，再点“保存为预设”。同名时按钮明确变为
“覆盖同名预设”；加载不会改变系统音量、低音炮、分频或声道校准。

- `dsp.presets {}` 返回 `items:[{name,config}]`。
- `dsp.preset.save {"name":"我的声音"}` 保存当前运行配置的16段输入EQ、启用状态和主增益。
  同名返回EEXIST；显式 `overwrite:true` 才覆盖。最多8份，名称1–48 UTF-8字节，不含控制字符。
- `dsp.preset.delete {"name":"我的声音"}` 删除指定预设，不改当前调音。
- 将预设的 `config` 传给 `dsp.config` 应用并保存；实时试用可加 `persist:false`。
- 自定义预设独立存入 `product/v1/dsp-presets`；重置、撤销普通设置均不删除预设。

## 音频抢占与恢复

电话音频建立后，普通音乐、DSP 与 TTS 让出音频设备。DSP 被通话打断时
`lastError=-EINTR`，页面明确说明原因；通话结束后用户重新播放，不自动惊扰播放。
非通话播放者各自持有并释放蓝牙音乐暂停引用，失败路径不会释放其他播放者的持有。
BT/DSP 在占用 ES8388 前通过语音服务释放唤醒麦克风，退出后恢复。
HFP 当前使用远端语音门控，尚不是经过声学校准的 AEC 方案。B53 电脑未枚举免提端点；用户暂缓手机配合测试，通话抢占本轮实机验收仍待完成。

## 实时音质修复（2026-10-18）

板载TTS逐句音质变化已通过“实际PCM截取→文件回放→实时修复”对照定位：
源PCM正常，故障发生在流式播放供数。NuttX上层在DEQUEUE前清零APB的`nbytes`，
旧TTS与蓝牙后端却用该字段扣队列长度，导致软件误以为一直有数据待播。

两套后端现保存每块提交长度，归还时按保存值扣减。TTS还在协议等待前按完整块
补充静音余量，覆盖模型加载、合成ACK和PCM事件等待；先取已就绪数据，避免
无必要的静音插入，写入失败向上传递。没有改采样率或强行把44.1kHz标成48kHz。

B47实时TTS记录到5次DMA供数间隙，最大946ms；B50同样三句间隙归零，用户确认
“正常稳定了”。同一份实际PCM的文件回放正常；蓝牙参考录音也经用户试听确认
一致正常。实际音乐PCM与FFmpeg独立解码差异RMS约1.7 LSB，44.1/48kHz同源对照用户听感无明显差别；仍觉得偏尖细，尚未完成音乐声学验收。B52已去掉临时诊断并保留修复。低音扬声器尚未接入，当前关闭bassEnabled，DSP保持全频输出。

## 可复现验证

在 `/root/openvela/organize-20261017` 执行：

```sh
python3 candidate/tools/nyabula_core/tests/plugin_upload_test.py \
  candidate/app/nyabula_core build-workspace/apps/netutils/cjson/cJSON \
  build-workspace/apps/crypto/mbedtls/mbedtls
python3 candidate/tools/nyabula_core/tests/dsp_player_test.py \
  candidate/app/nyabula_core build-workspace/apps/netutils/cjson/cJSON
python3 candidate/tools/nyabula_core/tests/audio_queue_test.py \
  candidate/app/nyabula_core
python3 candidate/tools/nyabula_core/tests/voice_keepalive_test.py \
  candidate/app/nyabula_core
python3 candidate/tools/nyabula_core/tests/bt_arbitration_test.py \
  candidate/app/nyabula_core
python3 candidate/tools/nyabula_core/tests/bt_recovery_test.py \
  candidate/app/nyabula_core
python3 candidate/tools/nyabula_core/tests/bt_audio_test.py \
  candidate/app/nyabula_core build-workspace/apps/external/libfluoride-sbc/libfluoride-sbc
cmake -S candidate/app/nyabula_dsp -B dsp-host -DNYADSP_SANITIZE_TEST=ON
cmake --build dsp-host -j2
ctest --test-dir dsp-host --output-on-failure
```

产品编译使用现有 `build-product-amp4-platform`，新增依赖补丁见 `patches/README.md`。
改 Kconfig 配置后先 `refreshsilent`，再运行 `bash rebuild-platform.sh` 重新生成头文件；
只改 C 源码时增量 `cmake --build build-product-amp4-platform -j4`。
插件硬件接口与图标清单见 [Plugin SDK](../tools/nyabula_plugin/README.md)。
真实手机接听/挂断和听感验证需要用户配合；Windows音频端点测试不替代这些操作。

## B54 实时调节与旁通对照

界面约160ms合并一次最新修改；请求串行执行，输入和拖动不锁住。
处理中仍在用旧实例时，新实例按实际输出延迟加2048帧预热，再交叉淡化。
预览期间不写SQLite；“保存设置”才持久化，离开页面后预览仍有效，重启回到已保存值。

B54播放中连续7次有效16段更新，处理帧持续增加，蓝牙欠载与丢包增量均0。
首轮测试的前三次请求发生在Windows输出端尚未进入streaming时；它们是空闲配置，
没有算作实时播放证据。

用户报告旁通听感像8/16kHz后，B53同一12秒测试音作false→true→false对照：
A2DP始终44.1kHz，SAI主时钟始终12.288MHz，SCO收发0，欠载0。
旁通不重开设备、不更改SRC；它会去掉用户当时158Hz/+8.5dB的低频补偿。
此次旁通对照未测得降采样。随后B54在旁通关闭时又出现电脑卡顿后音质突变，
重新播放恢复；这次故障不能用取消EQ解释，需单独处理断流恢复。

客户端已移除“日光”，保留星云/经典/仪器。旧日光选择自动迁为经典，仅改主题项，
不清除设备、登录和成员凭证。


## B55 断流恢复与调音保留

旧A2DP欠载路径只将playing清零，随后复用原输出；公共DSP不足一块的残留还可能
让“总排队字节为0”的判断一直不成立。新路径根据实际提交到硬件的队列与欠载计数
重开输出，重置SRC/DSP/SBC，并重新预缓冲；等待期间保留语音服务的扬声器占用，
避免唤醒麦克风抢改时钟。短写/打开失败与通话抢占继续走原有错误和所有权路径。

`bt.status.audio` 增加 `mediaRecoveries`、`mediaLastUnderrunMs`、`mediaLastRecoveryMs`
（自启动单调毫秒），以及实际解码头的 `mediaSbcRate/Channels/Bitpool/Blocks/Subbands`。
欠载计数与成功重开分别记录，避免把“尝试恢复”当作已恢复。

宿主恢复状态机、失败重开、持有期间拒开和DSP开关回归通过；B55启动及界面关闭、
重置、恢复自定义预设与重启持久化均通过，用户原16段与主增益-6.5dB逐字段保持。
B56临时诊断在250/600包时各注入1秒间隙，板上欠载2次、重开2次；第二次重开后
处理344064帧，error=0，实际SBC仍44100/2ch/bitpool35。被动采样时播放已结束，
恢复依据为计数/时间戳/处理帧和dmesg，听感反馈另记实测日志。B57已移除故障注入，
恢复正式候选；原16段、主增益-6.5dB、音量44与命名预设再次核对一致。
