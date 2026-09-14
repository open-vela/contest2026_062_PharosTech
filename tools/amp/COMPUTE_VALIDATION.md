# AMP 计算服务验收与板测证据（Draft）

日期：2026-09-14。本文记录隔离RAM镜像的实验结果，不把它们当作本PR已交付的业务RPC。
接口和部署边界见 [COMPUTE_INTEGRATION.md](COMPUTE_INTEGRATION.md)。

## 已完成的证据

| 编号 | 内容 | 结果与限制 |
|---|---|---|
| R004/R005 | OV5647连续取帧 | 1080P、900帧、30.592fps、sequence_gaps=0；不是ISP成品图像 |
| R008/R009 | 曝光/增益/AWB | 已得到可辨文字画面，主要绿偏改善；不是自动曝光/画质完整验收 |
| R012/R013 | Melo CPU/NPU对照 | 同输入数值一致性与实际NPU执行；固定音素前端，不含通用G2P |
| R019 | SFace特征 | 3组样例各10次，ONNX/NPU余弦最低0.999977668；warm中位14.113ms，不含检测/对齐 |
| R030–R032 | LLM加载与中文/工具输出 | 保留OP-TEE后加载成功；官方token输入与保留XML标记通过，未真实调用天气工具 |
| R034/R037 | LLM性能/前缀复用 | NPU950MHz提升decode；system-only缓存恢复缩短prefill，限已测fixture |
| R038/R039 | 三模型同驻与并发 | 两次启动各三轮七组合，另有热重载；各worker完成且退出0 |
| R039 MIC | 实际模拟MIC文件进入ASR | 9.856秒录音耗时1.252秒，ret0；识别文本不符合预定句，质量未判通过 |

模型与库保持原第三方许可；本PR未包含权重、二进制runtime、个人录音或生物特征模板。
R019仅公开样例，不是实际主人/陌生人评测，也不代表活体防伪。

## 三模型资源基线

K7 4GiB，Linux四A72，openvela四A53；ASR CPU1线程、Melo prefix4线程、RKLLM4CPU。
NPU performance 950MHz，CPU动态调频（R038采样816–2208MHz）。
ASR5.6115秒固定音频；TTS3.970612秒音频；LLM37输入/61生成tokens，逐请求清KV。
下面取R038三次中位数，未计算的另两路仍常驻。

| 并发计算 | ASR秒 | TTS秒 | LLM秒 | LLM首回调秒 |
|---|---:|---:|---:|---:|
| ASR | 0.774 | — | — | — |
| TTS | — | 1.631 | — | — |
| LLM | — | — | 2.540 | 0.165 |
| ASR+LLM | 3.019 | — | 2.910 | 0.348 |
| ASR+TTS | 1.203 | 2.180 | — | — |
| LLM+TTS | — | 1.662 | 4.202 | 1.814 |
| 三路 | 1.445 | 2.147 | 4.672 | 2.278 |

R039重复：三路中位0.960/2.193/4.683秒，LLM首回调2.300秒。
ASR存在调度波动；LLM+TTS的增加主要在首回调之前，内部调度原因尚未trace确认。
摄像头没有参与这些矩阵；不能将结果写成“四路全部同时运行”。

- 冷顺序全部READY观测窗口27.13秒；冷并行16.07秒（各一次，不是严格统计加速比）。
- 热文件缓存并行重载约8.04秒，不可当冷启动。
- R038空闲MemAvailable=2445716KiB（2.33GiB）；R039加载采样最低2104448KiB（2.01GiB）。
- 空闲PSS：ASR62684/TTS148816/LLM513536KiB；LLM生命周期VmHWM849488KiB。
  CPU进程PSS与SDK/NPU分配不是同一统计口径，不能简单相加得到全系统总耗用。
- 单独计算CPU时间中位ASR0.78/TTS2.96/LLM6.69 CPU秒；三路约11.86 CPU秒，
  窗口约4.7墙钟秒，平均约2.5个A72核。前后快照含轮询开销。
- 最高温度采样66.538°C；未测功耗瓦数/焦耳。采样最低可用内存不是连续精确峰值。

## 镜像与原始证据索引

这些镜像仅作RAM试验，不在PR中分发，不改普通NuttX槽或eMMC。

| 镜像 | 字节数 | SHA256 |
|---|---:|---|
| R038 multi.itb | 35295744 | `313f4c699e0120d6fa6e795a454db98b4fd0c40b0e706559cb040976b6598b53` |
| R039 multi-cold.itb | 35296768 | `a2d670534d74e5112714f522ca96d1fd2389369306d8a4e1cca147cf6ad2894a` |

RAM启动分别 `bootamp 60000000 21a9200` / `bootamp 60000000 21a9600`。
共用Image SHA256 `a951d905d09572b00fbde2be218c95ba8653232bbed186aa946a72078eba0dc2`；
媒体DTB SHA256 `54ac8db093c265e122797eae08694c5d812206c4c72c6657bf05255f45dd11a6`。
这些媒体DTB带额外配置，不等同于本PR的基础DTS构建产物。

原始日志在实验工作目录 `tmp/multimodel-20260914/R038/`、`R039/`：
`*.SEQ`首组worker输出、`*.LOG`热重载及采样、`LOAD.LOG`冷加载采样、`MIC.LOG`真实录音推理。
通过普通NSH cat串口取回，保留命令回显和提示符；`analyze.py`只解析，不重写原始数据。
本PR未携带上述本地目录；详细实验过程另存工作目录 `实测日志.md`。

## Draft 转正式前的验收清单

### 本PR改动验证（2026-09-14）

- 新增 `TopologyTest.test_board_reserves_optee`：修复前失败
  `AssertionError: unexpectedly None : BL32 memory must not enter the Linux allocator`；修复后通过。
- 在唯一构建机隔离目录执行 `python3 tools/amp/test_amp_topology.py`：14/14通过，
  包括编译DTB/FIT的测试，未跳过；`test_manifest_layout.py`：2/2通过。
- `cmake -S tools/amp/nyampd -B out/nyampd`、`cmake --build out/nyampd -j4`，
  `ctest --test-dir out/nyampd --output-on-failure`：2/2通过。
- `python3 tools/amp/test_application_pty.py . out/nyampd/nyampd`：
  `NYAMP_APPLICATION_PTY_PASS`。主机输出online=16是构建机CPU数量，不是板子拓扑。
- 用实际K7 SDK基础DTS包含本PR的AMP dtsi，经AArch64预处理与dtc编译；
  `fdtget -t x out/draft.dtb /reserved-memory/optee@48400000 reg`输出
  `0 48400000 0 1000000`，属性包含`no-map`；`validate_amp_layout.py --dtb out/draft.dtb`通过。
- 本次未重新构建完整Linux/NuttX固件或重刷板；板上行为证据来自前述同一保留区修复的实验镜像，
  不把本轮静态DTB验证写成新PR镜像已板测。

### 尚待完成

此清单是未完成工作，不是假接口或占位成功返回；未实现服务不得宣告ready。

### 可复现依赖与基础

- [x] AMP基础控制协议与health/info已存在主分支。
- [x] OP-TEE16MiB保留区修复已有板上因果对照；本PR纳入基础DTS。
- [ ] 将媒体Linux配置、OV5647/CIF修正按许可与单一职责整理；不复制GPL实现进Apache驱动。
- [ ] 外部RKLLM/RKNN/sherpa/ORT版本、模型下载/校验/许可、可复现构建和更新文档齐备。
- [ ] 大数据buffer分配/归还、generation失效、背压与超时实测通过。

### 已选三模型与相机正式接口

- [ ] LLM任意文本动态分词、流式输出、取消、工具执行回灌；同句柄串行与缓存失效测试。
- [ ] ASR实时PCM、partial/final、VAD结束、静音/噪声/数字/人名与真实MIC准确率。
- [ ] TTS通用G2P、长句分段、首PCM、取消、播放结束区分、无悬空PCM buffer。
- [ ] Camera常驻订阅、release、sequence/时间戳、消费者慢速/断开、RAW格式与画面质量。
- [ ] 四路摄像头+LLM+ASR+TTS并发，含buffer峰值、长稳、温度与失败恢复。

### 仍缺接口（用户本次明确保留）

- [ ] rPPG：帧时序/ROI质量、窗口、invalid输出、参考设备对照；不声称医疗用途。
- [ ] 认主：主动授权注册/删除、检测对齐、真实主人/陌生人评测、质量不足与未知人拒识。
- [ ] 语音唤醒：自定义词表入口、官方演示词、KWS/VAD、预录缓存、防TTS自唤醒。
- [ ] 声纹独立接口及真实MIC评测；不能把人脸认主当声纹已完成。

Core中的路由、产品状态、权限和主动任务继续由Core维护，本Draft不另建架构。
保持Draft直到约定范围与验收完成；CI全绿、此清单被更新均不构成代理合入授权。
