# 唤醒词（KWS）与主人声纹（speaker verification）— Linux 计算域后端

本目录给 `nyampd` 增加两件事，均走它已经链接的 sherpa-onnx 1.13.8 C API，不引入新运行时：

| 文件 | 内容 |
|---|---|
| `nyamp_voice.h` | 公共 `Status`/`Stop`（与 `nyamp::models::Status`、线上 `nyamp_model_status_e` 同序；合树后定义 `NYAMP_VOICE_WITH_MODELS` 即为同一类型） |
| `nyamp_kws.{h,cpp}` | 常驻关键词流：`Load(模型目录)` → 反复 `AcceptWaveform(16 kHz float32 窗)` → 回调 `KwsDetection{keyword, keyword_id, start/end/trigger_sample, score}` |
| `nyamp_speaker.{h,cpp}` | `SpeakerEmbedder::Embed` → L2 归一化向量；`Enrolment`（平均 + 成对余弦剔除离群）；`Verify` → 余弦分；`PackTemplate/UnpackTemplate`（"NYVP"，half 精度，192 维 = 400 字节，单条 RPMsg 装得下） |
| `nyamp_kws_file_test.cpp` / `nyamp_speaker_file_test.cpp` | 文件驱动测试，走与产品相同的类；打印 `NYAMP_*_FILE_PASS real_inference=1 mic_capture=0` |
| `keywords.txt` | **推荐的关键词文件**（81 行，sha256 `d4315e99…85137b`），部署时放进 KWS 模型目录 |
| `PROTOCOL.md` | 线上协议提案：KWS = service 10，SPEAKER = service 11（9 留给 BLOB） |
| `eval/` | 证据可复现脚本：TTS 批量合成、音频准备、扫参、声纹统计、全部关键词候选集 |

**第三方模型文件一律不进仓**，只在构建机 scratch 目录；来源/许可/sha256 见文末清单。

> ⚠️ **本文所有唤醒词正样本都是 TTS 合成语音（synthetic）**，没有任何真人说"你好 openvela"的录音。
> 表中的检出率只能用来**给方案排序**，不是产品指标。真人录音、远场、板上 aarch64 数据都必须重测（见"必须重测"）。

## 1. 结论

### KWS
- **模型**：`sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20`，chunk-16 变体，fp32 或 int8 均可（精度一致）。
  它是唯一一个**同一词表里既有拼音（声母+韵母）又有英文 CMU 音素**的官方 KWS 模型，"你好 openvela"这种句中切语言的短语只有它能直接表达。
  纯中文 wenetspeech-3.3M 只能把 openvela 写成拼音近似（held-out 中文检出 72 %，boost 提到 3 时真人语音出现 2 次误唤醒，且完全不支持英文短语），纯英文 gigaspeech-3.3M 只能做 "Hello open vela"（67–72 %）。
- **关键词**：`keywords.txt`（候选集 C）。每行 = 前缀 × 尾部变体，全部映射到两个 label：`@nihao_openvela`（id 0）、`@hello_openvela`（id 1）。
- **参数**：`keywords_threshold=0.10`，`keywords_score=2.0`，`max_active_paths=16`，`num_trailing_blanks=1`，1 线程。已写成 `KwsConfig` 默认值。

### 声纹
- **模型**：已有的 `3dspeaker_speech_campplus_sv_zh-cn_16k-common.onnx`（CAM++，192 维）。
- **注册**：4–5 次唤醒词，每次一个 embedding；`min_kept=3`，绝对门限 `min_mean_cosine=0.45` + 相对门限 `max_below_median=0.15`。
- **验证阈值（初值）**：余弦 **0.50**；"仅主人可执行"的敏感操作用 **0.55–0.60**。阈值放在控制域（NuttX），不写死在计算域。

## 2. KWS 是怎么调出来的（方法比数字更值得保留）

一开始照官方 text2token 习惯写 `n ǐ h ǎo OW1 P AH0 N V EH1 L AH0`，库默认参数，**几乎全部漏检（4/24）**。排查时同时列了四个假设并一次性验证：

| 假设 | 验证 | 结果 |
|---|---|---|
| H1 beam 太窄，长路径被剪 | `max_active_paths` 4/8/16/32 | **成立**：集 C 中文 4→8→16 路 = 44 %→72 %→91 %，CPU 仅 +3.8 % |
| H2 短语太长/"vela" 尾部拖垮平均概率 | 只测前缀、只测尾部 | **成立**：前缀 22/24，尾部是弱点 |
| H3 TTS 与模型不匹配（证据本身无效） | 用同一批 TTS 声音说模型自带关键词（你好军哥/小爱同学/你好问问） | **不成立**：18/18 检出 → 合成语音可作证据，难点是这个短语本身 |
| H4 "好"+"open" 连读吞掉 `OW1`/`ōu` | 去掉该音素的变体 | **成立，影响最大**：前缀 12/24 → 22/24；英文 "hel**lo o**pen" 同样共用一个 `OW1` |

关键手法：**KWS 模型本质是个 3M 的流式 transducer，可以直接当识别器跑**（`sherpa-onnx` 命令 + 同一组 encoder/decoder/joiner），看它把唤醒词"听成"什么，再据此写变体——先读、再改，不盲扫。读出来的事实：
- "你好"几乎总是 `n ǐ h ǎo`；
- "open" 一半走英文音素 `(OW1) P AH0 N`，一半走拼音 `(ōu) p ēn`（说完"你好"后模型倾向留在拼音路径）；
- "vela" 极不稳定：`V EH1/AE1/IY1/EY1 L AH0`、`Z EH1 L AH0`、`f èi l ā`、`z ài l éi`、`j iā l éi`……（普通话没有 /v/）。
- 还需注意尾部 flush：chunk-16 编码器要短语结束后约 0.4 s 的后续音频才吐出最后几个音素（实测触发延迟中位 0.40 s，chunk-8 为 0.34 s）；麦克风流天然满足，文件测试要补静音。

为防止对 TTS 过拟合，把 37 个 Kokoro 音色按说话人**对半分**：一半用于设计变体（design），另一半只用于验收（held-out）。三个候选集：
- **A**：只含语音学上讲得通的尾部（v→w/f，e→éi/ēi/èi，英文四种元音）。
- **B**：A + 模型"听到"的全部尾部，含真实词（`z ài l ái`=再来、`z ài l e`=在了）。
- **C**：A + 只保留**不是普通话词**的数据驱动尾部（`z ài l éi`、`j iā l éi`、`z ài l ā`、`f ǎn l ā`、`Z EH1 L AH0`、`V AE1/EH1 L IY0`）。← 推荐

### 2.1 held-out 说话人（合成），zh-en chunk-16 fp32，thr 0.10 / boost 2.0

| 关键词集 | beam | 中文 144 | 英文 72 | 8 kHz 音译 29 | 真人语音 15.7 min 误唤醒 | 普通混淆句 112 | 刻意仿冒句 64 |
|---|---|---|---|---|---|---|---|
| A | 8 | 54 (37.5 %) | 60 | 9 | 0 | 6 | 16 |
| A | 16 | 94 (65.3 %) | 68 | 14 | 0 | 7 | 30 |
| B | 8 | 122 (84.7 %) | 69 | 9 | 0 | 7 | 48 |
| B | 16 | 141 (97.9 %) | 72 | 14 | 0 | 10 | 54 |
| **C** | **16** | **131 (91.0 %)** | **72 (100 %)** | 14 | **0** | 10 | 50 |
| wenetspeech（拼音集） | 8 | 104 (72.2 %) | — | 4 | 0（boost 3 时 2） | 0 | 26 |
| gigaspeech（BPE 集） | 8 | — | 48–52 (67–72 %) | — | 0 | 0 | — |

误唤醒按句子拆开（每句 8 个音色）：
- 普通混淆句——"你好"、"你好小爱同学"、"你好世界…"、"你好欧文…"、"你好，open the door"、"open source"、"你好维拉，吃饭了吗"、"hello open ai"、"open the window"、"welcome to the villa"、"我们去开会吧"——**三个集全部 0/8**。只有 "hello, open the velvet box" 7–8/8、"你好，欧佩克宣布减产" 集 B/C 2/8。
- 刻意仿冒句：`open fella` / `open the valley gate` / `open 飞了` 三个集都触发（3M 音素模型分不清 vela/fella/valley，属固有局限）。
  **"你好，朋友们再来一杯"：A 1/8，B 7/8，C 3/8**——这就是选 C 不选 B 的原因：B 的"再来"尾部会让日常句子误唤醒。
  "你好，盆栽来了 / 欧喷再来 / open 再来" 是我按模型听感故意造的近音句，B、C 均 8/8。
- threshold 0.05–0.25 对结果几乎没影响；boost=3 且 threshold≥0.25 时检出反而下降（集 C thr 0.25：中文 84.7 %）。当前工作点是**召回受限**而非误唤醒受限。

### 2.2 选定配置在全部合成正样本上（集 C / beam 16 / 0.10 / 2.0）

| 集合 | 检出 |
|---|---|
| 中文短语 308（干净） | 279 (90.6 %) |
| 英文短语 154（干净） | 146 (94.8 %) |
| 中文 + 粉噪 ≈10 dB SNR | 243 (78.9 %) |
| 英文 + 粉噪 ≈10 dB SNR | 133 (86.4 %) |
| 按文本：你好，open vela / 你好open vela（无停顿）/ open 维拉 / open vayla / Hello open vela / Hello open vayla | 107/117 · 107/117 · 30/37 · 35/37 · 111/117 · 35/37 |
| 按语速 0.85 / 1.0 / 1.2 | 108/111 · 207/225 · 107/114 |
| 按引擎：Kokoro 37 音色 / **Melo 1 音色** | 421/444 (94.8 %) / **4/18 (22 %)** |
| 同一文件重复触发 | 0 |
| int8（encoder+joiner） | 中文 276/308，英文 148/154，真人误唤醒 0 |
| chunk-8 变体 | 中文 273/308，英文 146/154，真人误唤醒 0 |

Melo 那一个音色（1.2 s 说完整句、44.1 kHz 声码器）基本检不出，说明**检出率强烈依赖说话人**，再次提醒这些只是合成证据。

### 2.3 x86 主机资源（Xeon Silver 4314 @2.4 GHz，1 线程；**aarch64 数字必须上板测**）

| 项 | fp32 c16 | int8 c16 | fp32 c8 |
|---|---|---|---|
| RTF（942 s 真人语音） | 0.0232 | 0.0222 | 0.0342 |
| beam 4 → 16 的代价 | 0.0223 → 0.0232 | | |
| 加载 | 0.57 s | 0.69 s | 0.57 s |
| RSS：进程空载 → 加载后 → 单流连续跑 358 s 后 | 8.4 → 51.5 → 59.7 MB | 8.4 → 47.2 → 54.9 MB | 8.4 → 51.7 → 55.4 MB |
| 触发延迟（trigger − 短语结束）min/中位/max | 0.24 / 0.40 / 0.61 s | | 0.24 / 0.34 / 0.44 s |

单流连续跑 6 分钟 RSS 没有持续增长（59.7 MB 封顶）。`KeywordSpotter` 每次触发后重建 stream，时间戳原点由本类掌握，也避免常驻数日后 float 秒的舍入误差。

**`score` 字段**：sherpa-onnx 1.13.8 的 C API 不导出声学概率（阈值在库内部比较），所以 `KwsDetection::score` 恒为 -1、协议里 `HAS_SCORE` 位恒 0；字段保留，等运行时支持后不用改线格式。起止样本偏移可用（40 ms 分辨率），实测短语长度 1.04–3.4 s、中位 1.52 s，正好喂给声纹。

## 3. 声纹

### 3.1 整句余弦矩阵（真人公开样本，3 人 14 段，不去静音）

同一人：fangjun 0.70–0.86，leijun 0.77–0.90，liudehua 0.68–0.78。
不同人：fangjun↔leijun 0.08–0.30，fangjun↔liudehua −0.04–0.14，**leijun↔liudehua 0.22–0.44**（最难的一对，两位男声）。
完整矩阵见构建机 `out/spk-matrix.txt`（`--matrix` 可重现）。

### 3.2 产品流程仿真（真人）

注册：把每人的 `*-sr-*.wav` 去静音后切成 1.2/1.6/2.0/1.4 s 的片段（模拟唤醒词长度），取 3–4 段平均。
验证：把 `*-test-sr-*.wav`、`lei-jun-test.wav`（目标）与 Obama、英文双人对话、VAD 样本、KWS 测试句等（冒充者）切成定长片段，hop = 一半。

两个公开文件的标签不可信，已在统计中处理并**两种口径都报**：
- `0-four-speakers-zh.wav` 就是用这三个人的录音拼的（对 leijun/liudehua/fangjun 分别打出 0.81/0.69/0.60）→ 从冒充者中剔除。
- `lei-jun-test.wav` 是发布会录音，夹有主持人/观众/音乐 → "清洗"口径把连续 ≥2 个低于 0.30 的目标片段视为非雷军而剔除。清洗用的是被测模型本身，所以**清洗后的拒真率偏乐观**，未清洗的则是偏悲观的上界。

| 验证片段 | 阈值 | 误接受 FAR | 拒真 FRR（清洗） | 拒真 FRR（未清洗上界） |
|---|---|---|---|---|
| 1.5 s | 0.45 | 2/2209 (0.09 %) | 8/227 (3.5 %) | 22.1 % |
| 1.5 s | **0.50** | 1/2209 (0.05 %) | 9/227 (4.0 %) | 22.4 % |
| 1.5 s | 0.55 | 0/2209 | 14/227 (6.2 %) | 24.2 % |
| 1.5 s | 0.60 | 0/2209 | 22/227 (9.7 %) | 27.1 % |
| 3 s | 0.50 | 1/1059 (0.09 %) | 5/113 (4.4 %) | 20.0 % |
| 3 s | 0.55 | 0/1059 | 5/113 (4.4 %) | 20.0 % |
| 5 s | 0.50 | 1/603 (0.17 %) | 7/71 (9.9 %) | 17.9 % |
| 5 s | 0.55 | 0/603 | 7/71 (9.9 %) | 17.9 % |

等错误率（清洗口径）：1.5 s = 1.3 %（阈值 0.39），3 s = 2.7 %，5 s = 6.0 %（样本数 71，统计噪声大于长度效应）。分数分布：目标均值 0.69/0.74/0.73，冒充者均值 0.14/0.15/0.16、最大 0.51/0.52/0.52。

**样本极小**：只有 3 个注册人，目标片段 80 % 来自同一人（雷军），冒充者多为异语种/异性别。FAR 明显偏乐观。

### 3.3 同短语（text-dependent）合成实验——阈值不能照搬 3.2 的原因

37 个 Kokoro 音色各自用 4 条唤醒词注册，用另外 4 条验证，互为冒充者（148 目标 / 5328 冒充）：

| 阈值 | FAR | FRR |
|---|---|---|
| 0.50 | 7.6 % | 0 |
| 0.55 | 3.8 % | 0 |
| 0.60 | 1.7 % | 0 |
| 0.65 | 0.64 % | 0 |
| 0.70 | 0.28 % | 1.4 % |

冒充者分数（均值 0.29、p95 0.53、最大 0.79）远高于 3.2 的自由文本真人冒充者：**说同一句话 + 同一声码器会抬高相似度**。合成目标分（均值 0.88）也因 TTS 近乎确定性而虚高，所以此表不能直接定阈值，只说明一件事：**产品阈值必须用真人说同一句唤醒词的录音来定**，预计会落在 0.50–0.65 之间。初值取 0.50（真人 1.5 s：FAR 0.05 % / FRR 4 %），敏感操作 0.55–0.60。

### 3.4 注册离群剔除

往每个注册里混入 1 条别人的唤醒词：

| 策略 | 合成同短语 1332 例：剔除外人 / 误剔自己 | 真人 2 例 |
|---|---|---|
| 仅绝对门限 0.45 | 1193 (89.6 %) / 0 | 2 / 0 |
| 绝对 0.45 + 相对 0.10 | 1330 / **72** | 2 / 0 |
| **绝对 0.45 + 相对 0.15（默认）** | **1325 (99.5 %) / 0** | 2 / 0 |
| 绝对 0.45 + 相对 0.20 | 1299 / 0 | 2 / 0 |

绝对门限无法再提高：真人 1.2–2.0 s 片段的自洽度（cohesion）只有 0.58–0.70。相对门限只在多于 `min_kept` 段时修剪，绝不会让注册失败。

### 3.5 资源（x86，1 线程）

| 片段 | 提取耗时 | RTF |
|---|---|---|
| 1.5 s | 26 ms | 0.017 |
| 3 s | 45 ms | 0.015 |
| 5 s | 75 ms | 0.015 |

加载 0.40–0.51 s；RSS 空载 8.4 MB → 加载后 68.7 MB；短音频实验峰值 84 MB（3.2 的 128–143 MB 峰值来自测试程序把 5 分钟 wav 整段读入，不是模型）。
声纹模板 float32 → half → float32 往返余弦 = 1.000000，400 字节。

## 4. 必须重测 / 未决事项

1. **真人唤醒词录音**（最重要）：≥20 人 × ≥10 次，含中式英语口音、儿童、不同距离。用第 2 节的方法（先用 KWS 模型当识别器听一遍 → 据此增删 `keywords.txt` 尾部变体 → held-out 验收）重定关键词与阈值。团队需先统一 "vela" 读法（/ˈvɛlə/、/ˈviːlə/、/ˈveɪlə/、"维拉"），目前四种都覆盖。
2. **误唤醒**：15.7 分钟负样本只能说明"没有明显问题"，产品指标需 ≥24 h 家庭环境音（电视、多人聊天）得到"次/天"。
3. **板上（RK3576 aarch64）**：KWS 常驻 RTF/CPU 占用/RSS、A53 小核绑定是否够用、int8 在 ARM 上的收益、声纹单次耗时；本机只做到 aarch64 交叉编译链接通过（`-Werror`），无 qemu-user，未运行。
4. **远场/混响/回声**：本评估只有干净 TTS 和粉噪；喇叭自播放时的回声、3 m 距离均未覆盖。
5. **声纹阈值**：用真人同短语数据重画 3.3；补同性别中文冒充者；补跨天/感冒/小声等会话差异；确认 1.0–1.5 s 唤醒词片段是否够用，不够则"唤醒词 + 后续指令"拼接到 ≥3 s 再验。
6. **zh-en KWS 模型许可未声明**：压缩包内无 README/LICENSE，文档页与可访问的 model card 均未写；同发布页的两个旧模型 README 写明 Apache-2.0。**随作品分发前须向 k2-fsa 确认。**
7. 协议与 `nyampd` 集成（环形缓冲、ASR attach、模板缓存）只有提案（`PROTOCOL.md`），未实现。
8. 声纹属生物特征个人数据：仅存设备 `/config/voice/`，不上云，须提供删除。1–2 s 唤醒词上的声纹只能做个性化/软门控，不是认证因子。

## 5. 复现（构建机 `/root/openvela/voice-eval-20260920`，只写该目录）

```sh
S=$PWD/sherpa-onnx-v1.13.8-linux-x64-shared
cmake -S src -B build-x64 -DCMAKE_BUILD_TYPE=Release -DNYAMP_VOICE_EVAL=ON \
  -DNYAMP_SHERPA_INCLUDE=$S/include/sherpa-onnx/c-api \
  -DNYAMP_SHERPA_LIBRARY=$S/lib/libsherpa-onnx-c-api.so
cmake --build build-x64 -j2          # 共享机器，内存紧，勿超过 -j2

# 模型目录用固定文件名（软链即可）：
#   mdl/zhen-c16/{encoder,decoder,joiner}.onnx tokens.txt keywords.txt
#   mdl/speaker/speaker.onnx

sh src/eval/gen_tts_jobs.sh $PWD/tts                      # 生成合成任务单
build-x64/nyamp_voice_tts_batch kokoro kokoro-int8-multi-lang-v1_1 tts/jobs-kokoro.tsv
sh src/eval/prep_audio.sh $PWD                            # 16 kHz、补静音、加噪、列表
KWS_EXTRA="--max-active-paths 16" sh src/eval/kws_sweep.sh $PWD \
  $PWD/build-x64/nyamp_kws_file_test zhen-c16 setC "0.10 0.25" "2.0" "test_zh test_en neg_real"

build-x64/nyamp_kws_file_test mdl/zhen-c16 --quiet --expect-none --list lists/neg_real.txt
build-x64/nyamp_speaker_file_test mdl/speaker --manifest out/spk-real-big.tsv --probe-seconds 1.5 > t.txt
awk -v clean=1 -v drop=0-four-speakers-zh -f src/eval/spk_stats.awk t.txt
```

aarch64：同上，加 `-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++`，库指向
`/root/openvela/speech-fallback-20260912/runtime/sherpa-onnx-v1.13.8-linux-aarch64-shared-cpu/lib/libsherpa-onnx-c-api.so`。

## 6. 第三方文件清单（均不在仓内）

发布页前缀 `R = https://github.com/k2-fsa/sherpa-onnx/releases/download`。

### 随产品部署的模型

| 文件 | 来源 | 许可 | sha256 |
|---|---|---|---|
| `sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2`（32.9 MB） | `R/kws-models/` | **未声明，须确认**（见 4.6） | `68447f4fbc67e70eee3a93961f36e81e98f47aef73ce7e7ca00885c6cd3616a6`（与发布页 checksum.txt 一致） |
| ├ `encoder-epoch-13-avg-2-chunk-16-left-64.onnx`（12.0 MB）→ `encoder.onnx` | 同上 | 同上 | `540ff509ed89bd22afe04bf7049a54bb1c95c6d8a18742ea9691910cdb5f859e` |
| ├ `encoder-…-chunk-16-left-64.int8.onnx`（4.6 MB，可选） | 同上 | 同上 | `408bbd740838c42d5bf6d1c5b80b3c88b616c7860b92d980328b5b068c76ae48` |
| ├ `decoder-epoch-13-avg-2-chunk-16-left-64.onnx` → `decoder.onnx` | 同上 | 同上 | `63a22dd60f40fff082ac3e09afa507f6787da36df76ded2fbe145fa233e22c21` |
| ├ `joiner-epoch-13-avg-2-chunk-16-left-64.onnx` → `joiner.onnx` | 同上 | 同上 | `76f7a24ed0c08633af14b2ee377f747af880d3b65eeba2cd3f31f3380fb73e8d` |
| ├ `joiner-…-chunk-16-left-64.int8.onnx`（可选） | 同上 | 同上 | `190d4067b4cc20b72a42a1916e69d92052000fb7051a427ebb1bc72a69207dc1` |
| └ `tokens.txt` | 同上 | 同上 | `2d3f32311f9b692b964da3c90e830258d3e78e013cb0c992dbfb15cd5a1a71b0` |
| `3dspeaker_speech_campplus_sv_zh-cn_16k-common.onnx`（28.3 MB）→ `speaker.onnx` | `R/speaker-recongition-models/`（原始：阿里 3D-Speaker / ModelScope `iic/speech_campplus_sv_zh-cn_16k-common`） | Apache-2.0 | `f682b514c05d947ee3fa91cd6ec6c5c7543479a128373fa29b1faedccd21fd11` |

### 仅评估用（对比模型、运行时、TTS）

| 文件 | 来源 | 许可 | sha256 |
|---|---|---|---|
| `sherpa-onnx-kws-zipformer-wenetspeech-3.3M-2024-01-01.tar.bz2` | `R/kws-models/` | Apache-2.0（包内 README） | `b2f7c89690dc8ce4c6ed6afeab7cd800c36ad1421fb6b6302b4a4b194cf7f35f` |
| `sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2` | `R/kws-models/` | Apache-2.0（包内 README） | `f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a` |
| `sherpa-onnx-v1.13.8-linux-x64-shared.tar.bz2`（含 bin + c-api.h） | `R/v1.13.8/` | Apache-2.0（内含 ONNX Runtime，MIT） | `c0bdb7907d3a74bba1d55d22bf4d9fa75586cf1530614ebe88a27b9118e015c4` |
| `kokoro-int8-multi-lang-v1_1.tar.bz2`（147 MB，TTS，103 音色） | `R/tts-models/`（原始 `hexgrad/Kokoro-82M-v1.1-zh`） | Apache-2.0（包内 LICENSE） | `a1e94694776049035c4f2c6529f003aaece993c76aae9a78995831c3c4dcafc6` |
| `vits-icefall-zh-aishell3.tar.bz2`（TTS，8 kHz，174 音色） | `R/tts-models/` | 模型由 icefall 导出（Apache-2.0）；训练数据 AISHELL-3（Apache-2.0） | `ab468db3a3308cdd861495e0db2f25d79418a0c00639f74944c7cdf5dd8c6ec1` |
| `vits-melo-tts-zh_en/model.onnx`（本机已有） | `R/tts-models/vits-melo-tts-zh_en.tar.bz2`（原始 MyShell MeloTTS） | MIT（包内 LICENSE） | `bf30582eb1b012250a35b1a4a80e7dfbcf8485e7bb9de0d95efbbeef0e4ad86d` |

### 评估用公开录音（非模型，未再分发）

`R/asr-models/`：`lei-jun-test.wav`、`Obama.wav`、`test_silero_vad.wav`、`zh.wav`、`en.wav`、`itn-zh-number.wav`；
`R/speaker-segmentation-models/`：`0-four-speakers-zh.wav`、`1/2/3-two-speakers-en.wav`；
`R/speaker-recongition-models/`：`fangjun-sr-1..3`、`fangjun-test-sr-1..2`、`leijun-sr-1..2`、`leijun-test-sr-1..3`、`liudehua-sr-1..2`、`liudehua-test-sr-1..2`；
以及两个旧 KWS 模型包内的 `test_wavs/`。真人负样本合计 941.6 s（15.7 min）。
