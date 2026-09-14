# 板载 LLM、ASR、TTS 与摄像头对接说明（Draft）

更新：2026-09-14。面向 Nyabula Core 与 Linux 计算域的接入开发。
本文区分团队仓已实现代码、隔离镜像板测能力、拟议接口；接口名称不是已发布 ABI。
本 Draft 不另建 Agent，不实现 Core 的产品编排，也不宣称模型接口已经交付。

## 1. 状态与交付物

| 能力 | 已验证 | Core 可调用的正式接口 | 主要缺口 |
|---|---|---|---|
| AMP 控制通路 | 四 A53 openvela + 四 A72 Linux，RPMsg health/info | 团队仓已有 health/info | 大数据 buffer、业务 RPC、取消完成语义 |
| LLM | MiniCPM5 中文、XML 工具输出、前缀缓存、常驻与并发 | 未实现 | 动态 tokenizer、请求队列、增量事件、工具回灌 |
| ASR | 中文 Zipformer 固定样本与真实 MIC 文件推理 | 未实现 | PCM 流传输、端点检测、识别质量验收 |
| TTS | Melo CPU prefix + NPU vocoder，固定音素输入快于实时 | 未实现 | 通用文本前端、分句、PCM 交接、取消与播放闭环 |
| 摄像头 | OV5647 CSI0，1080P 连续 900 帧，无序号跳变 | 未实现 | 常驻帧服务、消费者生命周期、图像预处理 |
| rPPG | 选型/方案阶段 | 未实现 | ROI、时序窗口、质量分数、有效性评测 |
| 认主 | 主机检测/对齐参考；SFace NPU 特征板测 | 未实现 | 实时画面、主动注册、模板管理、未知人拒识 |
| 语音唤醒 | 接口需求阶段 | 未实现 | KWS、可配置词表、VAD、预录缓存、唤醒事件 |
| 声纹 | 独立能力预留，不等于人脸认主 | 未实现 | 注册、特征、阈值与真实 MIC 测试 |

本 PR 当前携带 OP-TEE DTS 修复、修复回归测试及本对接文档。
模型权重、vendor 动态库、Linux 摄像头驱动改动、板测 worker 与业务 RPC 尚不在本 PR。
克隆本分支只能复现仓内 AMP 基础与测试，不能直接得到下面的隔离媒体镜像。

## 2. 所有权与部署前提

- openvela：产品/Core 状态、权限、任务编排；普通音频固件已由 ES8388 采集/播放。
- Linux：模型句柄、NPU 调用、CSI/V4L2 buffer。摄像头连续流不经过 Core 逐帧转发。
- 隔离媒体镜像由 Linux 独占 SD/eMMC host；不能让两个 OS 同时挂载同一文件系统。
  本轮只读 eMMC，模型位于 SD；不能把普通 NuttX 的 `/data` 操作原样用于同时运行的 AMP。
- 模型进程相互隔离：RKLLM 与 RKNN/ORT 使用不同运行库路径，避免同进程符号冲突。
- NPU 调频是计算域共享状态，由计算域统一管理；Core 不逐请求直接写 sysfs。
- 现有 AMP 是协作式分区，不是硬件安全隔离；不能承诺任一 OS 独立重启无影响。

必须保留 Linux `reserved-memory` 中的 OP-TEE `[0x48400000,0x49400000)`、16 MiB、`no-map`。
遗漏时大模型分配曾在 `clear_page` 触发 SError；不是等待更久可以解决的加载慢。
其余内存地址与 CPU 归属见 [README](README.md) 和 [OWNERSHIP](OWNERSHIP.md)。

团队仓基础 profile 不等于媒体板测 profile：后者额外启用 Linux 存储、CPUFreq、
OV5647/CIF 等，并带对应驱动修正。配套 Image 与 DTB 必须同批构建。

## 3. 已存在的控制协议

源码：`protocol/nyamp_protocol.h`、`nyampd/nyampd_core.cpp`、`app/nyampctl`。
wire version 1，little-endian，40 字节头，RPMsg MTU 496，inline 最大 456 字节。
头已有 request_id、deadline_ms、generation、消息类型；这些字段存在不等于取消已实现。

当前已实现 HEALTH service=1：opcode 0 READY 事件、1 health、2 info。
已预留 service：NPU=2、ASR=3、TTS=4、VISION=5、MEDIA=6、HOME=7。
**LLM、KWS、声纹尚无独立 service ID；本文件不自行分配编号，也不复用不相干编号。**
基础 daemon 的业务分发只接受 health/info，其他请求返回 unsupported。
板上曾运行增强 NPU 探针的镜像，不代表当前主分支已有同样的 RPC。

当前可直接执行：

```text
nyampctl health
nyampctl info
```

音频、图像与长 token 数组不能硬塞 inline，更不能传另一个 OS 的虚拟地址或 Linux fd。
大数据共享区目前仅预留，allocator、描述符编码与 release 协议均未实现。
下一阶段先确定共享对象 ID、generation、offset、length、格式、归还时机，再开放业务 RPC。
协议定义见 [protocol/README.md](protocol/README.md)。

## 4. LLM 接入

### 已测配置

- 模型 `openbmb/MiniCPM5-1B`，W4A16/GRQ，RKLLM toolkit/runtime 1.3.0。
- 文件 `MCP5W4.RKL`，875760324 字节，SHA256
  `1282395749fd24baa12e55c4623475c298710caf56b2b0e804e62ab26aa25f4f`。
- Linux 测试挂载路径 `/data/MEDIA912/MCP5W4.RKL`；不是产品固定配置路径。
- 4 A72，CPU mask 0x0f，双 NPU 核模型，context=2048，NoThink。
- `librkllmrt.so` 与 `libgomp.so.1`，依赖 glibc/libstdc++/libgcc；仅由 Linux worker 加载。

### 输入输出约定

原模型官方 tokenizer/template 生成完整 token 数组，再设置 `RKLLM_INPUT_TOKEN`。
初始化后 `rkllm_set_chat_template(handle, "", "", "")`，禁止 SDK 再套一次模板。
`skip_special_token=false` 保留 XML 工具标记；消费者处理结束符 `<|im_end|>`。
原始 `RKLLM_INPUT_PROMPT` 曾输出无意义片段；当前是外部分词绕过，不是 SDK tokenizer 已修复。

测试使用构建机预生成的 `.ids`，**动态任意中文消息分词尚未部署**。
`rkllm_run` 同步执行，增量 callback 返回文本/token；同一句柄不能无约束并发调用。
工具调用输出只证明参数生成，天气查询等真实执行和结果回灌仍由 Core 接入。

固定系统/工具定义置于 prompt 前部，变量消息在后。连续同类请求可复用公共前缀；
跨任务后已验证恢复 system-only cache，再传完整 token 数组。模型/模板版本变化需失效缓存。
基准测试每次清 KV，不能把其耗时当有前缀缓存的工具请求耗时。

## 5. ASR 接入

- sherpa-onnx 1.13.8，ORT 1.28.2，CPU provider，中文 streaming Zipformer 14M
  （2023-02-23），INT8 encoder/decoder/joiner 合计 25305928 字节。
- SD 测试目录 `/data/MEDIA912/ASR/`：`ENC.ONX`、`DEC.ONX`、`JOIN.ONX`、`TOKEN.TXT`。
- 接收 16000 Hz、mono；传给 sherpa 的 samples 为归一化 float32，S16 输入转换除以32768。
  不能把 stereo 或48kHz原始字节直接当16kHzmono。
- recognizer 常驻，每次会话独立 stream；默认实测单线程最省 CPU。
- 顺序：CreateOnlineStream → AcceptWaveform → InputFinished → 在 IsReady 时 Decode →
  GetResult → DestroyStream。保留 recognizer，不逐请求重新加载模型。
- 已测文件模式额外前补0.5秒、后补0.8秒零样本，属于当前测试fixture方式，
  不是产品 VAD 的静音阈值，也不代表已测在线 partial 延迟。

真实 MIC 文件已进入 ASR，但输出不符合预定句、现场口述未确认，识别质量仍待独立验收。
普通固件录音和 AMP 文件推理跨重启完成；**尚无 AMP 实时 MIC→ASR 流式链路**。

普通 openvela 录音入口（不要在最小 AMP 固件假设这些 app 已启用）：

```text
audioctl mic main
nxrecorder
device /dev/audio/pcm_in0
recordraw /tmp/input.pcm 1 16 16000 0
stop
q
```

`stop` 在录音后人工发送；`main` 是板载模拟 MIC，不是耳机或 USB MIC。
不指定 `pcm_in0` 曾报 `No suitable Audio Device found`。

## 6. TTS 接入

当前选择 Melo zh_en：CPU encoder/flow（`PREFIX.ONX`）→ NPU masked vocoder（`MASK512.RKN`）。
ORT 1.28.2 四线程；RKNN runtime 2.3.2。第三方库保持独立许可，不随自有代码改标 Apache。

- 已测前端输入为95个含blank音素IDs和tones，原句：
  “你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？”
- `X.BIN`、`TONES.BIN` 是 int64 fixture，不是任意文本服务。
- CPU prefix 输出 latent/speaker。NPU latent 以 FLOAT32/NHWC 提交，内部做 FP16；
  speaker 同步提交，valid_mask 严格标明有效帧。不能只补零而不屏蔽卷积尾部。
- 固定512 latent帧bucket，上限约5.944秒PCM；本句342帧、3.970612秒。
  超出bucket时需要分句或新增已验证bucket，不截断音频伪成功。
- 输出单声道44100Hz float PCM；音频消费者按实际格式转换/播放。
  NPU outputs 必须在处理或复制完之后 release，异步消费者不得持有已释放指针。
- 常驻测试不加载 `DECANY.ONX` CPU oracle，不把对照模型内存当产品占用。

尚缺通用文本 G2P、标点/中英混说、PCM chunk 交接与 nxplayer/音频设备闭环。
模型完成不等于扬声器播放完成，也不等于用户听见首音频。

## 7. 摄像头接入

真实器件为 CSI0 的 OV5647 H型120度镜头，I2C4地址0x36，25MHz模块晶振、两条CSI lane；
不是资料中的 IMX415。自动对焦暂不作为前置条件，VCM0x0c多阶段无应答，原因未判定。

板测使用 Rockchip SDK Linux V4L2/CIF：

1. 通过 sysfs/media 拓扑找 OV5647 subdev 和 `bus_info=platform:rkcif-mipi-lvds` 捕获节点；
   `/dev/videoN` 和 `/dev/v4l-subdevN` 动态编号不能写死。
2. 设 sensor ACTIVE 格式；驱动需具备 g_frame_interval、get_mbus_config、
   V4L2_CID_LINK_FREQ 与正确CIF裁剪。相关SDK源码修正尚未纳入本PR。
3. 捕获为 `VIDEO_CAPTURE_MPLANE` + `V4L2_PIX_FMT_SBGGR10`。
   `RKCIF_CMD_SET_CSI_MEMORY_MODE` 设低位16bit模式并GET读回；实测值1。
4. 1080P实际 stride=3840、sizeimage=4147200，RAW10存在little-endian16bit低10位；
   不能按packed10或RGB/YUV读取，也不能忽略驱动返回的stride/bytesused。
5. REQBUFS/MMAP → QBUF → STREAMON → poll/DQBUF → 消费 → QBUF；退出 STREAMOFF 后释放映射。
   原始帧复用前必须完成消费者访问或复制，携带sequence及V4L2时间戳语义。

实测连续900帧30.592fps、sequence_gaps=0；并非摄像头与三模型四路同时运行测试。
1080P RAW约4.15MB/帧，仅原始数据约127MB/s，不能用小RPMsg包搬运。
历史900帧探针在RAM缓存90帧Bayer8片段，额外约178MiB，不是最终实时消费者内存预算。

已测曝光1000行、模拟增益128（16为1x）、sensor AWB改善绿偏。
这些值是现场诊断配置，不是所有场景的产品默认；预览可辨文字不等于ISP画质验收。
rPPG尤其要记录曝光/增益/AWB变化与时间戳，不能将显示用gamma处理后的视频默认为原始信号。

## 8. 面向 Core 的逻辑接口草案（全部待实现）

以下是对接讨论用的操作与数据，不是现有 C 函数、RPC opcode 或 HTTP URL。
编码、service ID、错误码和兼容版本在实现提交中集中定义，不在 Core 散落私有原型。

| 模块 | 拟议操作 | 输入 | 输出/事件 |
|---|---|---|---|
| LLM | load / generate / cancel / unload | model_id、完整token数组引用、生成参数、请求上下文版本 | token/text增量、finish_reason、耗时、明确结束事件 |
| ASR | open / push_audio / finish / cancel | sample_format、rate、channels、PCM引用、sequence | partial/final文本、已消费音频位置、结束事件 |
| TTS | synthesize / cancel | UTF-8文本、voice、rate；前端未就绪时不得假接收任意文本 | 带采样格式的PCM引用、最后chunk、合成结束 |
| Camera | open / configure / subscribe / release / close | 模式、可支持控制项、消费者请求 | frame_ref、格式/stride、sequence、时间戳、丢帧信息 |
| rPPG | start / stop | camera订阅、授权、ROI/时间窗配置 | bpm或invalid、质量、有效窗口、原因；不输出未经质量检查的数值 |
| Owner | enroll / identify / delete / cancel | 用户主动同意、模板ID、合格帧引用 | owner/unknown/insufficient_quality；版本与匿名分数 |
| KWS | configure / start / stop | 词表版本、PCM流；支持范围内的自定义唤醒词 | detected事件、keyword_id、时间戳、分数 |
| Speaker | enroll / verify / delete | 主动同意、模板ID、PCM引用 | match/unknown/insufficient_quality，与人脸结果分开 |

共同需要确认：request_id唯一关联、deadline采用现有共享counter口径、generation变化使旧引用失效、
cancel请求与最终完成事件分别确认、buffer由谁拥有及何时release、错误只结束对应请求。
当前wire只定义flag，并未实现上述cancel流程。
同一请求只产生一个最终结果；这些是待验收的约定，不是现有能力保证。

Core负责“什么时候听/说/识别谁/触发工具”；计算域负责“执行模型、交付结果与资源状态”。
识别成功不直接触发硬件动作，LLM输出工具参数也不越过Core权限与工具执行层。

## 9. 未实现功能的边界

- rPPG：需要稳定人脸/皮肤ROI和有效时序，运动、遮挡、低光时返回不可用。
  当前没有板上准确性证据；不作为医疗测量、诊断或紧急告警依据。
- 认主：参考YuNet检测/5点 → 112×112对齐 → SFace128维L2特征 → 模板匹配。
  已测SFace特征中位14.113ms，不含取帧/检测/对齐；不是完整认主延迟或准确率。
  相机实时接入、主人/陌生人阈值、质量筛选未完成；不能用第一张脸自动注册主人。
  当前无活体能力，不用于开锁/支付；模板本地保存、用户可删除，不进入普通日志和代码仓。
- KWS与VAD分开：KWS决定开始交互，VAD/超时决定音频段结束；ASR不是唤醒器。
  自定义唤醒词需明确模型/词表支持范围。赛事演示保留“你好，openvela / Hello, openvela”。
  预录环形缓冲、静音时长、最大录音长度及TTS播放期间防自唤醒尚未实现/验收。
- 声纹独立于人脸认主；不能用音色相似度作为安全身份保证。

## 10. 性能与交付验收

实测摘要与待办见 [COMPUTE_VALIDATION.md](COMPUTE_VALIDATION.md)。
三模型常驻稳定仍有约2.33GiB可用内存；冷并行加载峰值采样最低2.01GiB。
LLM+TTS并发主要延后LLM首字，Core后续需按交互场景决定排队与取消策略；本文不实现调度器。
摄像头buffer、rPPG、人脸/KWS/声纹尚未纳入四路或更多路并发资源验收。

## 11. 现有板测入口与可复现资料位置

以下为当前工作目录中的实验源码位置，**不在本PR源码树内**，不是克隆本仓即可执行的安装步骤：

| 能力 | 本地实验目录 | 入口/依赖 |
|---|---|---|
| LLM | `tmp/minicpm5-board-20260913/` | `rkllm_probe.cpp`、官方token fixture生成脚本、外部RKLLM1.3 |
| ASR | `tmp/media-amp-20260912/` | `model_bench.c`、sherpa/ORT；`model-bench asr 1 /run/multi` |
| TTS | `tmp/melo-npu-20260912/` | `vocoder_probe.c`；`vocoder-probe hybrid /data/MEDIA912 /etc/vocoder-inputs /run/multi 512` |
| 摄像头 | `tmp/media-amp-20260912/` | `camera_capture.c`、SDK头/驱动；`camera-capture /run/camera 1920 1080 300` |
| 同驻矩阵 | `tmp/multimodel-20260914/` | `build.sh`、`run.sh`、`analyze.py`；各worker `NY_BENCH_WORKER=1` |

worker stdin `RUN`触发固定样本，`STOP`退出，stdout `NY_READY`/`NY_DONE`为实验标记。
它们不是线上协议、没有通用请求参数/取消保证，不应由Core解析实验stdout作为正式接入。
摄像头探针还含只读对焦诊断，正式实现不照搬探针的多余I2C访问。

唯一构建机实验根 `/root/openvela/`，上述目录同名；不包含私人SSH地址/密钥配置。
实验镜像依赖既有SDK/kernel构建目录与模型，不是自包含发布包。
后续按单一职责提交Linux适配层、模型runtime依赖说明和正式接口测试，再更新本表状态。
