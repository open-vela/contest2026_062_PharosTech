# 语音链路：唤醒 → ASR → LLM → TTS（2026-09-20，计算域一半已实现）

状态：★ = 有上板/主机实测；【主机实测】= 构建机 x86_64 实测；其余为读码结论或设计。
**Linux（计算域）一半已实现并通过主机测试；openvela（控制域）一半已实现、主机测试与交叉语法检查通过（第 9 节）；整条链未上板。**

## 1. 已实现 / 未实现

| 项 | 状态 | 证据 |
|---|---|---|
| 协议：`TTS_SYNTH_TEXT`、ASR `END`/`ATTACH_KWS`/PARTIAL 标志、KWS 服务(10)、SPEAKER(11) 占号 | 已实现（编解码 + 单测） | `nyamp_protocol_test` 通过；线格式见 `tools/amp/protocol/README.md`「语音」 |
| `NYAMP_SLOT_CAPTURE`（0x120000 / 256 KiB）新槽 | 已写入 `rk3576_shmem_layout.h` | `test_shmem_layout.py` 仍通过（只比对头部常量）；**控制域固件须用同一版头文件重编** |
| `nyampd_asr`：流式增量解码、partial 文本、endpoint 标志、PUSH 与 attach 两种供音方式 | 已实现 | 脚本后端 10 组 + 真模型【主机实测】：`test_wavs/0.wav`、`1.wav` 经线上流式识别的最终文本与 `nyamp_asr_file_test` 的整段参考逐字一致 |
| `nyamp_sherpa.cpp` 由"整段喂完再解码"改为流式，整段后端改为建在流式之上 | 已实现 | 改造前后 `nyamp_asr_file_test` 对两条 wav 的输出逐字节相同 |
| `nyampd_tts`：文本 → G2P → 分句 → 逐单元合成 → PCM 窗口；句间/窗口粒度取消；永不截断 | 已实现（x86 用脚本声码器） | 9 组；真 G2P【主机实测】金标准句 95 id 与 `X.BIN` 逐位一致、342 帧、`valid_samples=175104`、4 个窗口 |
| `nyampd_kws`：常驻流、10 s 环、DETECTED{id, 起止/触发偏移}、ASR attach | 已实现 | 脚本后端 4 组 + 真模型【主机实测】10 条正样本全部检出、9 条负样本（含 50 s 真人语音）零触发 |
| 模型按逻辑目录名 `asr`/`tts`/`kws` 经 BLOB 交付 | 已实现 | `nyampd_asr_test`「logical model name through BLOB」（进程内控制域应答器） |
| HEALTH capability bit4/5/6、`info` 的 `asr= tts= kws=` 行 | 已实现 | 单测 + 读码 |
| 无语音运行时也能工作 | 已实现 | 不给任何运行时的 x86 构建：ctest 9 通过 1 跳过，三服务回 `UNSUPPORTED` |
| aarch64 交叉构建（rkllm + sherpa + ort + rknn，动态、无 RUNPATH） | 已实现，**未运行** | `file`=ARM aarch64；NEEDED 的库 initramfs 里都已有，`build_compute_initramfs.sh` 不用改 |
| TSan | 已跑 | 四个新测试 0 条 ThreadSanitizer 警告；ASan+UBSan 同样干净 |
| 线上端到端 | 已实现 | `tools/amp/test_voice_flow.py`：纯 Python 客户端 ↔ 真实 Dispatch/服务，SOCK_SEQPACKET + 文件映射的 arena |
| SPEAKER（声纹）服务 | **未实现**（仅占号 11） | `tools/amp/voice/nyamp_speaker.*` 库在，未接入 |
| 控制域：`ny_voice_capture/pump/sm/play/eyes`、`voice.*` 话题、nyampctl 的 kws/asr/tts 子命令 | 已实现，**未上板、未整编** | 第 9 节：主机测试 4103+235 项、交叉语法检查全过 |
| NPU 声码器路径（`nyamp_melo.cpp`） | 只做了 aarch64 编译链接 | 需上板 |
| 板上 RTF、内存、三模型并发、真人唤醒率 | **未测** | 需上板 / 真人录音 |

## 2. 槽位共享契约（结论：精确切分，不做半双工）

原计划"ASR 输入与 TTS 输出交替用 `SLOT_SHARED`"被放弃，原因是实测后才看清的两点：唤醒词流**永不停止**；
`SLOT_SHARED` 现在还是每次模型拉取（BLOB）的窗口，一句合成语音也会占满它。共用 = 拉模型和播报期间机器人是聋的。
arena 4 MiB 里原有 2.9 MiB 未分配，切分零成本：

| 槽 | 偏移 / 大小 | 用途 | 同一时刻的属主 | 强制手段 |
|---|---|---|---|---|
| `NYAMP_SLOT_SHARED` | `0x001000` / 1 MiB | BLOB 窗口（控制→计算）**或** TTS PCM（计算→控制） | 一次拉取 或 一个 TTS 请求 | 同一把 `BlobClient` 属主标志：对方在用时回 `BUSY`（`nyampd_tts_test`「the shared slot」两个方向都测了） |
| `NYAMP_SLOT_CAPTURE` | `0x120000` / 256 KiB | 采集音频（控制→计算），4 个 1 s float32 窗 | KWS 流 或 一个 PUSH 式 ASR 请求 | 属主标志 + lease：KWS 流在跑时 PUSH 式 ASR_BEGIN 回 `BUSY`，此时 ASR 用 attach |

- lease 由 Linux 单边铸造：`generation<<32 | bit31 | 计数`，每个 grant、每个 TTS 窗口各一个。
  PUSH/RELEASE 的 lease、generation、范围、采样格式任一不符 → `INVALID`，不读内容。
- 采集侧：grant 覆盖整个槽、活到请求/流结束；PUSH 指其中子范围，**应答发出即已拷走**，范围立刻可重写
  （一个窗口或两个乒乓都行，无需逐窗 RELEASE）。
- 播放侧：**一次只有一个窗口在外**，回显同一 lease 的 RELEASE 之后服务才再写槽；30 s 无 RELEASE → `DEADLINE` 并归还槽。
- "说话时闭麦"现在只是控制域的防自唤醒策略，不再关系内存安全：SPEAKING 期间停发 KWS_PUSH，
  恢复时 `stream_sample` 照实继续（或置 `DISCONTINUITY`），计算域重置 KWS 解码器、偏移保持绝对。
- 是否采用"ASR 从偏移 N 接入 KWS 流"：**采用**。环的所有权足够简单——单写者（传输线程 append）、
  读者各持游标互不消费、环属于 KWS 流、读者持 shared_ptr 且流结束时看到 closed；落后超过 10 s 的读者丢最旧的并被告知 gap。
  `nyampd_audio_test` 在没有任何服务的情况下把这些性质钉死（含 1 写 2 读线程）。PUSH 方式保留给没有 KWS 流的场合（文件回放、调试）。

## 3. 板上要放的模型文件（控制域 `/data/models/` 下，三个逻辑目录）

**目录里只放下表文件**：逻辑目录名加载会把该目录下所有文件（≤3 层）都拉到 Linux 的 tmpfs。
文件名是固定的（后端按名打开），构建机上的原名见"来源"。均为第三方文件，**不进仓**。

### `asr/`（24.2 MiB）—— sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23，int8
来源目录：`/root/openvela/speech-fallback-20260912/models/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/`
（已按目标名软链在 `/root/openvela/voice-work/models/asr/`）

| 板上文件名 | 字节 | sha256 | 构建机原名 |
|---|---:|---|---|
| `encoder.onnx` | 21621684 | `1c556ea57cec304e55ec4b72e52c1cc098bb01476ed7d90f3de939fe126487b1` | `encoder-epoch-99-avg-1.int8.onnx` |
| `decoder.onnx` | 1888682 | `22f123bb8cba9b38974b3df18a3f45e7081f4985ebb2e075d9f21f618c468bbf` | `decoder-epoch-99-avg-1.int8.onnx` |
| `joiner.onnx` | 1795562 | `a7cf9d82757bdcf786059454495a9ca95e4bd7347f72473fc08d794475c36169` | `joiner-epoch-99-avg-1.int8.onnx` |
| `tokens.txt` | 48697 | `8b294db9045d6e5f94647f4c1eec1af4da143a75053c399611444b378ff966ac` | `tokens.txt` |

### `kws/`（12.5 MiB）—— sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20，chunk-16 fp32
来源目录：`/root/openvela/voice-eval-20260920/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20/`
（软链在 `/root/openvela/voice-work/models/kws/`）。**该模型许可未声明，分发前须向 k2-fsa 确认**（见 `tools/amp/voice/README.md` 4.6）。

| 板上文件名 | 字节 | sha256 | 构建机原名 |
|---|---:|---|---|
| `encoder.onnx` | 11976272 | `540ff509ed89bd22afe04bf7049a54bb1c95c6d8a18742ea9691910cdb5f859e` | `encoder-epoch-13-avg-2-chunk-16-left-64.onnx` |
| `decoder.onnx` | 759829 | `63a22dd60f40fff082ac3e09afa507f6787da36df76ded2fbe145fa233e22c21` | `decoder-epoch-13-avg-2-chunk-16-left-64.onnx` |
| `joiner.onnx` | 338154 | `76f7a24ed0c08633af14b2ee377f747af880d3b65eeba2cd3f31f3380fb73e8d` | `joiner-epoch-13-avg-2-chunk-16-left-64.onnx` |
| `tokens.txt` | 1928 | `2d3f32311f9b692b964da3c90e830258d3e78e013cb0c992dbfb15cd5a1a71b0` | `tokens.txt` |
| `keywords.txt` | 3913 | `d4315e9983245b861684646734e3411af77f58d601126885cbd44142da85137b` | 仓内 `tools/amp/voice/keywords.txt`（**用 LF 行尾的版本**；Windows 检出若成 CRLF，sha 会变，CRLF 能否被运行时接受未验证） |

### `tts/`（155.4 MiB）—— MeloTTS zh_en：CPU 前缀 + NPU 声码器 + 文本前端资产

| 板上文件名 | 字节 | sha256 | 构建机位置 |
|---|---:|---|---|
| `prefix.onnx` | 111376080 | `8f474e06bd3122c6aef67774a0be38f2a93c7d7445af85eee3af914ba55f8e73` | `/root/openvela/voice-work/models/tts/prefix.onnx`（整文件；板上现有的三段分片 `PFX00/01/02.BIN` 需拼回这一个文件，拼后核对 sha）【主机实测：x86 onnxruntime 加载 1.8 s，金标准句出 342 帧】 |
| `vocoder.rknn` | 39464514 | `aa85f171abb32162d35053ca4fa9f03858cdccf34b1800b600e719a2b06c039a` | `/root/openvela/melo-npu-20260912/models/masked512.rknn`（**推断**：与板上 `MASK512.RKN` 应为同一文件，上板前核对 sha） |
| `tokens.txt` | 655 | `d18664a7e12bd7ea1022ddaf951e534e136815016c5a809d6b64156bffb4369d` | `/root/openvela/g2p-work/assets/tokens.txt` |
| `lexicon.txt` | 6837671 | `7236884b02435ac5d10cf69b4be40a61b45aa676b5300f0e412f185748fee528` | `/root/openvela/g2p-work/assets/lexicon.txt` |
| `dict/jieba.dict.utf8` | 5071204 | `3043b77068e09c9904f27cad82f12b6ebe9dbdb5aeff3b25e45ab7f9c1122b55` | `/root/openvela/g2p-work/assets/dict/jieba.dict.utf8`（可缺：缺则退回正向最大匹配，"要不要"读 bu4） |
| `LICENSE` | 1053 | `88a50e5a02bbc2a5c2f084dc19da751aa97b1690f5fda76cd8005c8634d1ca70` | `/root/openvela/g2p-work/assets/LICENSE`（MIT，随资产分发） |

注意 `tts/tokens.txt` 与 `asr/`、`kws/` 的 `tokens.txt` 是三个不同文件，别放错目录。
三套合计 ≈192 MiB，连同 LLM 835 MiB 都进 Linux 的 tmpfs（`/tmp` 1536 MiB）；内存预算见 `amp-linux-20260920/out/MANIFEST.txt`。

nyampd（aarch64，含四个运行时）构建产物：`/root/openvela/voice-work/arm/nyampd/nyampd`，464288 B，
sha256 `0ffa6e6f…863688e`（随源码变动会变，以重编为准）；NEEDED：librkllmrt / libsherpa-onnx-c-api / libonnxruntime /
librknnrt / libstdc++ / libm / libgcc_s / libc，无 RUNPATH。**未在板上运行过。**

## 4. 板测：预期线上行为（全部未上板验证）

前置：固件与 nyampd 用**同一版** `nyamp_protocol.h` 和 `rk3576_shmem_layout.h`；模型按第 3 节放好。

1. `nyampctl health`：capabilities 应为 `0x7f`（health|llm|blob|chat|asr|tts|kws）。若是 `0x0f`，说明 nyampd 没链上语音运行时。
2. `nyampctl info` 新增一行，加载前：`asr=off:-2 tts=off:-2 kws=off:-2`（`-2`=NOT_READY，尚未 LOAD 过）。
   LOAD 成功后对应项变 `ready:0`；拉取中 `loading:…`；识别/合成中 `busy:0`；唤醒流运行中 `kws=listening:0`；
   LOAD 失败保持 `off:<线上状态>`（`-8` 后端打不开文件，`-2` 控制域没有该目录）。`none:` = 该服务没编后端。
   info 总共 452 字节，bootlog 尾巴会相应变短。
3. `ASR_LOAD "asr"`：先收到若干 `BLOB/EVENT_PROGRESS`（request_id = LOAD 的），再收到 LOAD 应答 OK。
   x86 上模型加载 ≈0.7 s【主机实测】；板上拉取 24 MiB 按 44 MiB/s★ 约 1 s。
4. 回放式 ASR（先于真麦克风）：`ASR_BEGIN`(16 B) → 应答带 NYBS：offset `0x120000`、capacity `0x40000`、format F32、
   lease 高 32 位 = generation。把 `test_wavs/0.wav` 按 1 s 窗写入 → `ASR_PUSH`(sequence 0,1,2…) → `ASR_END`(全 F)。
   预期：多个 `EVENT_PARTIAL`（文本逐步变长），最后一帧 flags 含 RESYNC|FINAL，文本
   `对我做了介绍那么我想说的是呢大家如果对我的研究感兴趣呢`，再 `EVENT_FINISH` status 0。
   `1.wav` → `重点想谈个问题首先呢就是这一轮全球金融动量的表现`。
5. `KWS_LOAD`（16 B 头全 0 + `"kws"`）→ `KWS_LIST` 应答 2 个 label：`nihao_openvela`(id 0)、`hello_openvela`(id 1)。
   `KWS_BEGIN`(16000,1,0,16000) → grant 同上槽。此后**所有**采集窗口只发 `KWS_PUSH`（带 `stream_sample`），应答回下一个期望位置。
   说"你好 openvela" → `EVENT_DETECTED` keyword_id 0，flags 通常 = 1（HAS_OFFSETS），`trigger-end` 约 0.3–0.6 s。
   flags = 0 时偏移无效，用 `trigger_sample`。
6. 唤醒后：`ASR_BEGIN`(24 B，flags bit1，`start_sample`=DETECTED 的 `end_sample`)，应答无 body；继续只发 KWS_PUSH。
   看到 PARTIAL 带 ENDPOINT(bit1) 或本地 VAD 静音 1.2 s 后发 `ASR_END`(当前 stream_sample) → FINAL 文本 + FINISH。
7. `TTS_LOAD "tts"`（约 155 MiB，拉取 ≈4 s + 加载 ≈5.7 s★）。`TTS_SYNTH_TEXT`（28 B 头 + UTF-8，≤428 B/块）发
   `你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？` → 预期 4 个 `EVENT_PCM`：offset `0x1000`、
   `valid_samples` = 44100, 44100, 44100, 42804（合计 **175104** = 342 帧×512），首窗 RESYNC、末窗 LAST，
   每窗须回 `TTS_RELEASE`（原样回显 40 B NYBS）后才有下一窗；`EVENT_FINISH` status 0、sequence 4、total_samples 175104。
   先把 PCM 落 WAV 用 nxplayer 听，再信播放线程。合成耗时预期 2.5–2.6 s★（旧测量，走新服务后需复测）。
8. TTS 进行中发 `BLOB PULL` 或逻辑名 LOAD → `BUSY`；反之亦然。TTS 进行中 `TTS_CANCEL` → 立即 FINISH(-6)，在外窗口作废。

## 5. 主机实测数字（构建机 Xeon Silver 4314，x86_64，1 线程推理；**不代表板上**）

- ASR 经线上流式：0.wav 5.61 s 音频，模型加载 715 ms；以 20 倍速推窗（每 1 s 窗间隔 50 ms）首个文本在 101 ms 出现、
  359 ms 全部完成，11 个 PARTIAL 帧；1.wav 5.15 s，13 帧，345 ms。最终文本与整段参考一致。
- KWS 经线上流式：加载 600 ms；116.3 s 音频（10 正 + 9 负，每段后补 1 s 静音）；正样本偏移与同事的逐文件评估一致
  （如 `kokoro-s000-v085-zhA`：start 0.32 s / end 2.04 s / trigger 2.40 s）。
- TTS 真 G2P + 脚本声码器：前端加载 206 ms；430 字节中英混合回复 → 7 个单元、2205 帧（25.6 s 语音）、30 个窗口，
  无一单元超 512 帧、声码器零拒绝。
- 真 `prefix.onnx` 在 x86 CPU（4 线程，一次性探针，不在仓内）：加载 1.8 s；金标准句 95 id → **342 帧**（与板上★一致）、146 ms；
  同一回复的 7 个单元 290–393 帧、78–112 ms/单元。56 符号预算在这组句子上余量充足（最大 49 符号 → 393 帧）。
- 反压：200 步脚本识别、无人读队列 → 送达 66 个 PARTIAL 帧，FINAL 与 FINISH 不丢，接收方文本仍逐字正确。

## 6. 实测发现的问题（不是本服务的 bug，但影响产品）

1. **KWS 长流里的偏移不可信 + 连续流误唤醒**【主机实测，用同事自己的工具可复现】：
   `nyamp_kws_file_test mdl/zhen-c16 --continuous --list <10 正样本后接负样本>` 会在负样本
   `neg/kokoro-s000-t04.wav`（"你好欧文，好久不见"）上触发，且 `start_s=-61.6`——"短语"在一分钟前。逐文件或只跑负样本都是 0 触发。
   看起来 sherpa 返回的时间戳取的是该假设**最早**的 token 而不是关键词自己的 token，所以只有解码器刚重置时偏移才对。
   同样现象：一条被漏检的正样本会在下一条上"补触发"，偏移指向前一条。
   本服务的对策：偏移不合理（`trigger-end`>2 s、短语 >6 s、start≥end）就不置 `HAS_OFFSETS`；误唤醒本身归
   `tools/amp/voice` 的关键词/阈值调优，逐文件评估得到的"普通混淆句 0/8"**不能直接当作常驻流的指标**。
   真模型测试因此排除了 t04，其余 9 条负样本作为**一条不间断的流**跑，零触发。
2. 被 `DISCONTINUITY` 丢掉的是环里**尚未读走**的采样。真实暂停以秒计没有影响；测试里两段之间留了 300 ms。
3. `nyampd_main.cpp` 的 `bootlog=` 原先在剩余空间不足时会让 `info_size` 超过缓冲区（snprintf 返回"本应写入"的长度）；
   加 `asr= tts= kws=` 行后更容易触发，已顺手改成按剩余空间截取。

## 7. 控制域那一半（最初设计；实现见第 9 节，与此处不同之处以第 9 节为准：默认 F32 窗口、保留 4 s 预录环、HALF 下无 VAD 打断）

NuttX 线程：`ny_voice_capture`（独占 pcm_in0 16 kHz S16★，RMS 门限 VAD；**S16 可直接 PUSH**，BEGIN flags bit0，省一半穿越非缓存区的字节）、
`ny_voice_pump`（采集 → CAPTURE 槽 → `KWS_PUSH`）、`ny_voice_sm`（状态机、提交 agent、驱动眼睛）、
`ny_voice_play`（消费 EVENT_PCM，f32→S16，写 pcm0 44.1 kHz★，写完回 `TTS_RELEASE`）。
预录环**不再需要放在控制域**：计算域的 10 s 环 + attach 已经保证唤醒词之后的指令不丢不重。

状态机：IDLE（KWS 流常开）—DETECTED→ LISTENING（ASR attach）—ENDPOINT 或静音 1.2 s 或 8 s 上限→ `ASR_END` → THINKING
→ SPEAKING（停发 KWS_PUSH 防自唤醒；VAD 打断 = `TTS_CANCEL` + 清 pcm0）→ 恢复 PUSH（`DISCONTINUITY`）→ IDLE。
Agent：新增非 static 包装 `ny_agent_voice_submit(conversation, text)` → `ny_agent_submit()`（单回合互斥，忙时 -EBUSY）；
回复经 `message_bus_pop_outbound` 整段给出。眼睛：listening→`curious`，thinking→`processing`，speaking→`happy`；
`eyes.expression` 是 1 s 同步确认，**不得在音频线程里调**。
首音延迟：LLM 整段生成 + TTS 首单元，估 4–7 s；TTS 服务已按句出窗，LLM 侧按句流水与本地"我在"应答音仍待做。

## 8. 复现（构建机，只写 `/root/openvela/voice-work`）

```sh
W=/root/openvela/voice-work; S=/root/openvela/voice-eval-20260920/sherpa-onnx-v1.13.8-linux-x64-shared
# x86，带 sherpa：ctest 10/10
cmake -S $W/wt/tools/amp/nyampd -B $W/build/host-sherpa -DCMAKE_BUILD_TYPE=Release \
  -DNYAMP_SHERPA_INCLUDE=$S/include/sherpa-onnx/c-api -DNYAMP_SHERPA_LIBRARY=$S/lib/libsherpa-onnx-c-api.so
nice cmake --build $W/build/host-sherpa -j4
T=/root/openvela/speech-fallback-20260912/models/sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23/test_wavs
cd $W/build/host-sherpa && LD_LIBRARY_PATH=$S/lib \
  NYAMP_ASR_MODEL=$W/models/asr NYAMP_ASR_WAVS=$T/0.wav:$T/1.wav \
  NYAMP_KWS_MODEL=$W/models/kws NYAMP_KWS_POSITIVE=$(cat $W/ref/kws-positive.list) \
  NYAMP_KWS_NEGATIVE=$(cat $W/ref/kws-negative.list) \
  NYAMP_G2P_ASSETS=/root/openvela/g2p-work/assets NYAMP_G2P_GOLDEN=/root/openvela/g2p-work/golden \
  ctest --output-on-failure
# x86，无任何运行时：cmake -S … -B $W/build/host-bare && ctest   （9 通过，g2p 无资产跳过）
# TSan：-DCMAKE_CXX_FLAGS="-fsanitize=thread" …；运行加 setarch -R（本机 ASLR 与 TSan 冲突）
# 线上流程：TMPDIR=$W/tmp python3 $W/wt/tools/amp/test_voice_flow.py $W/wt
# aarch64：D=/root/openvela/amp-linux-20260920/deps
NYAMP_RKLLM_ROOT=$D/rkllm NYAMP_SHERPA_INCLUDE=$W/deps/include/sherpa \
NYAMP_SHERPA_LIBRARY=$D/speech/libsherpa-onnx-c-api.so NYAMP_ORT_INCLUDE=$W/deps/include/ort \
NYAMP_ORT_LIBRARY=$D/speech/libonnxruntime.so NYAMP_RKNN_INCLUDE=$W/deps/include/rknn \
NYAMP_RKNN_LIBRARY=$D/rknn/librknnrt.so JOBS=4 nice bash <src>/tools/amp/nyampd/build_arm64.sh $W/arm/nyampd
# initramfs 照 amp-linux-20260920/out/MANIFEST.txt 的 build_compute_initramfs.sh 一步，换成这个 nyampd 即可（脚本未改）。
```

## 9. 控制域实现进度（2026-09-20 起，随做随记；未上板）

设计定稿见本节末"设计决定"。文件均在 `app/nyabula_core/`，前缀 `ny_voice_`。

- [x] `ny_voice_dsp.{c,h}` 纯函数：S16↔F32（双向同一比例 32768，往返逐值相等）、单声道→立体声、RMS、
  WAV 头生成/解析、预录环（绝对采样位置、多游标、落后告知 gap）、attach 偏移算术、空闲门限、
  44.1k→16k 多相重采样（160/441，48 抽头）、按句切分。
- [x] `ny_voice_sm.{c,h}` 状态机纯转移函数（事件 + 时间 → 状态 + 有序动作表）。
- [x] 主机测试 `tools/nyabula_core/tests/voice_test.{py,c}`：-std=c99 -Wall -Wextra -Werror -Wshadow -Wundef，
  普通 + ASan/UBSan 两遍，各 4103 项检查 0 失败【主机实测，构建机】。
  测试抓到并已修的两个真问题：①/32768 与 ×32767 不对称导致往返差 1；②噪声底在说话期间被语音抬高，
  一句 5 s 的话后半句会被判成"不响"。
- [x] `ny_voice_wire.{c,h}` 语音线上客户端（经回调收发：板上走 ny_compute 的 port，nyampctl 独立模式走端点，
  主机测试走 socketpair），不含任何 NuttX 头。头部带当前 generation（0=未知）：nyampd 重启后旧请求回
  `STALE_GENERATION`→`-ECONNRESET`，等待中发现 generation 变化也立即结束。
- [x] 主机测试 `tools/nyabula_core/tests/voice_wire_test.{py,c}`：服务端复用 `tools/amp/test_voice_flow.py` 里的
  harness（真实 Dispatch + AsrService/TtsService/KwsService，脚本模型），客户端是固件里那份 C。
  235 项检查 0 失败，ASan/UBSan 干净【主机实测】：PUSH 式 ASR、KWS 流 + DETECTED + attach（consumed=position-end）、
  DISCONTINUITY、偏移不可信时按 trigger 回退、FLAG_CANCEL 取消、TTS 多块文本 55 个窗口逐窗 RELEASE、
  播放中取消得 FINISH(-6)、S16 窗口、generation 变化。
  实测到的线上事实：`trigger_sample` 是**解码块**（1600 采样）末尾而不是 PUSH 窗口末尾；跳号的 ASR_PUSH 回 INVALID 但请求仍可 END。
- [x] `ny_voice_audio.{c,h}`：NuttX 音频节点的流式封装（RESERVE→CONFIGURE→GETBUFFERINFO→REGISTERMQ→ALLOCBUFFER→
  ENQUEUE/START，放音首块带 44 字节 WAV 头）。与蓝牙同事的 `ny_audio_stream.c` 同类，但那份只在
  `NYABULA_CORE_BT` 下编译且仍在改，语音链不依赖它；**两份日后应合并**。编译通过，未上板。
- [x] `ny_voice.{c,h}`：服务本体（任务 `ny_voice_sm` + 线程 `ny_voice_capture/pump/play/eyes`）、`voice.*` 话题、
  设置持久化（store 域 `voice`，已加入 `ny_product_store.c` 白名单）。编译通过，未上板。
- [x] `ny_compute.{c,h}` 扩展：`ny_compute_generation()`、`ny_compute_capabilities()`、`NY_COMPUTE_CAP_ASR/TTS/KWS`
  (bit4/5/6)、`compute.status` 的 capabilities 数组加 asr/tts/kws、端口数 4→8（语音常驻 3 个会话）、
  `ny_compute_start/stop` 里起停语音任务（`ny_product_runtime.c` 他人在改，故挂在这里）。
- [x] `app/nyampctl/nyampctl_voice.c`：`kws listen [秒]`、`asr file <wav>`、`asr mic [秒]`、`tts say <文本> [out.wav]`，
  与服务共用 `ny_voice_wire.c`。nyampctl 栈默认在开 VOICE 时升到 8192。
- [x] 构建接线：`app/nyabula_core/{Kconfig,Makefile,CMakeLists.txt}`、`app/nyampctl/{Kconfig,Makefile,CMakeLists.txt}`。
- [x] 交叉语法检查【构建机实测】：固件树 `product-amp-20260920` 的 aarch64-none-elf-gcc 13.4，真实 include 路径，
  `-Wall -Wshadow -Wundef -Werror -Wstrict-prototypes`，10 个文件 × 3 种 codec 方案 × 2 组选项全过；
  不定义 `CONFIG_NYABULA_CORE_VOICE` 时 `ny_compute.c/ny_product.c/nyampctl_main.c` 也过。**没有做过固件链接/整编。**
- [ ] 上板：全部未做，脚本见 9.4。

### 9.1 设计决定

**ES8388 采样率约束（读驱动得出，`nuttx/drivers/audio/es8388.c`）。** 放音、录音是两个实例，`es8388_reserve()` 允许
不同 stream_type 同时 reserve；但 `es8388_configure()` 先调 `es8388_check_peer_format()`：对端实例 `reserved` 且
`samprate != 0` 时，本端的 **采样率、位宽、声道数**任一不同即 `-EBUSY`。`samprate` 只在 reserve 时清零，
所以只要麦克风节点开着（16 kHz/16 bit/单声道），任何 44.1 kHz 放音（TTS、闹钟提示音 44.1k 立体声、flash 音乐、
蓝牙 A2DP）都配置不上——不只是 TTS 的问题，是**常驻唤醒与全机所有放音**的冲突。三种方案做成 Kconfig choice：

| 方案 | 录音 / 放音 | 说话时麦克风 | 其它播放者 | 依据 |
|---|---|---|---|---|
| `CODEC_HALF`（默认） | 16k 单声道 / 44.1k 立体声，**不同时** | 关闭 | 须先调 `ny_voice_speaker_claim()` | 唯一建立在板上已测事实（pcm_in0 16k 单声道、pcm0 44.1k）之上 |
| `CODEC_SHARED_16K` | 16k 单声道 / TTS 重采样到 16k 单声道 | 开着 | 同上 | 重采样器已主机实测；16k 单声道放音未上板 |
| `CODEC_SHARED_44K` | 44.1k 立体声录音、软件抽取到 16k / 44.1k 立体声 | 开着 | 44.1k 立体声的无需仲裁 | SAI 44.1k 立体声录音未上板 |

**★ 必须的集成补丁（不在本次改动内，因 `ny_product_media.c` / 蓝牙音频他人在改）：** HALF 与 SHARED_16K 下，
`ny_media_start()` 在 `nxplayer_setdevice()` 之前、蓝牙音频在打开 pcm0 之前要调 `ny_voice_speaker_claim()`，播完调
`ny_voice_speaker_release()`（与现有 `ny_product_bt_speaker_claim()` 同一位置、同一形状；语音未开时两者零成本）。
**不打这个补丁，voice.enable 期间音乐和闹钟提示音会 `-EBUSY`。** 默认 `NYABULA_CORE_VOICE=n` 且 `enabled=false`，不开语音不受影响。

**回声 / 打断策略。** 没有 AEC。SPEAKING 期间不向 KWS 推流：HALF 下麦克风本来就关了；SHARED 下默认也不推，
除非开 `NYABULA_CORE_VOICE_BARGE_IN`（主人宁可冒自唤醒风险也要语音打断）；即便如此，回复文本含 "openvela" 的那一句
照样闭麦。状态机对 SPEAKING 中的 WAKE/LISTEN 一律处理为：`TTS CANCEL` + 停放音 → LISTENING，所以 HALF 下的打断入口是
面板的 `voice.listen` / `voice.cancel`。恢复推流时带 `DISCONTINUITY`，`stream_sample` 照实继续。

**预录环留在控制域（4 s，S16，128 KB）。** 计算域 10 s 环解决"唤醒后指令不丢"，解决不了"门限开得晚、唤醒词前半截没推"。
门限（自适应噪声底 ×2.5、最低 RMS 150、2 s 拖尾）只决定静音要不要过链路；门一开先补推最近 1.5 s（不早于麦克风上次打开的位置），
LISTENING 期间强制开门。`NYABULA_CORE_VOICE_VAD_GATE=n` 可整个关掉。

**attach 位置**（`ny_voice_attach_sample`，主机实测）：`HAS_OFFSETS` 且 start<end≤trigger → `end_sample`；否则 `trigger−0.5 s`；
再夹到 [max(流头−10 s+0.5 s, 上次 DISCONTINUITY 位置), 流头]，因为服务对早于环的位置回 INVALID 而不是悄悄后移。
首次唤醒要先加载 ASR：先放"稍等"提示（`/data/voice/wait.wav`，没有就两声提示音），加载完从**流头**接入（HALF 下提示音期间麦克风是关的）。

**以主人身份提交 agent，不改 `ny_agent.c`。** 直接走公开的 `ny_agent_request(owner, "agent.chat")`，再用 `agent.run.get` 每 300 ms 轮询：
`succeeded/failed/cancelled/unknown` → 回复；`waiting_approval` → 说固定句"这个操作需要你在面板上确认一下"，之后继续跟踪该 run 10 分钟，
主人在面板批准后把结果说出来。5 分钟内的连续回合共用一个 `conversationId`（"那明天呢"能接上），超时换新，避免历史无限增长。
安全性靠 agent 自己的规则：有副作用的动作（principal 为空）一律 `pending`，语音只会"请求确认"，不会替主人确认。
打断思考（THINKING 中再次唤醒）= `agent.cancel` + 重新聆听；新回合提交遇 `-EBUSY/-EAGAIN` 在 10 s 内每 0.5 s 重试。

**模型懒加载与内存。** KWS 在语音启用时加载（12.5 MiB）；ASR 首次唤醒时；TTS 首次回复时（≈10 s，短提示不为此加载 TTS）。
加载/拉取进度在 `voice.status.models.*.{state,pulled,total}`。Linux 可用 ≈3.65 GiB★，LLM 进程 830 MiB + tmpfs 835 MiB，
三个语音模型 tmpfs 192 MiB + 运行时（TTS ≈200 MiB、ASR/KWS 估 <150 MiB，**未测**）≈ 2.2 GiB，放得下，所以默认**不卸载**
（`NYABULA_CORE_VOICE_UNLOAD_IDLE_S=0`）；设成 N 则空闲 N 秒后 UNLOAD TTS 与 ASR（文件留在 tmpfs，重载只是运行时加载）。
`/tmp` 1536 MiB 上限：835+192=1027 MiB。nyampd 重启（generation 变）→ 三个模型状态清零、在途回合按状态机 LINK_LOST 处理、KWS 流自动重建。

**线程与端口。** 一个 port 一次只路由一个 request_id，而一次语音请求的全部消息共用 BEGIN 的 id，所以 KWS/ASR/TTS 各占一个 port，
分属 pump / sm / play 线程，互不读对方的帧。跨线程取消用无应答的 `NYAMP_FLAG_CANCEL` 帧。眼睛走单槽邮箱 + 独立低优先级线程；
恢复 idle 前先读 `eyes.status`，表情已不是本服务设的那个就不动（agent 可能在本回合里被要求换了表情）。

### 9.2 defconfig 与开销

```
CONFIG_NYABULA_CORE_VOICE=y                  # 依赖 NYABULA_CORE_COMPUTE、_AGENT、_AUDIO、AUDIO（AMP 配置里都已是 y）
CONFIG_NYABULA_CORE_VOICE_CODEC_HALF=y       # 默认；另两个：_CODEC_SHARED_16K / _CODEC_SHARED_44K
# CONFIG_NYABULA_CORE_VOICE_BARGE_IN is not set   （仅 SHARED）
CONFIG_NYABULA_CORE_VOICE_VAD_GATE=y
CONFIG_NYABULA_CORE_VOICE_VAD_MIN_RMS=150
CONFIG_NYABULA_CORE_VOICE_WINDOW_MS=200
CONFIG_NYABULA_CORE_VOICE_SPEAKER=1
CONFIG_NYABULA_CORE_VOICE_CUE_DIR="/data/voice"
CONFIG_NYABULA_CORE_VOICE_UNLOAD_IDLE_S=0
CONFIG_NYABULA_CORE_VOICE_PRIORITY=100
CONFIG_NYABULA_CORE_VOICE_STACKSIZE=32768
CONFIG_EXAMPLES_NYAMPCTL=y
CONFIG_EXAMPLES_NYAMPCTL_STACKSIZE=8192      # 开 VOICE 后的新默认
```

RAM【构建机实测，aarch64 -O2 的 `size`】：HALF text 57.7 KB / data 19.9 KB / bss 326.7 KB（环 128 KB、输出缓冲 88 KB、窗口 32 KB、
提示音 35 KB、三个会话 25 KB）；SHARED_16K bss 445.9 KB、SHARED_44K 396.2 KB（重采样表 30 KB 等）。启用后另加堆：3 个 port ≈96 KB、
音频缓冲 ≈26 KB（录）+66 KB（放，仅说话时）；栈：sm 32 KB + play 16 KB + eyes 16 KB + capture 8 KB + pump 8 KB = 80 KB。
合计 HALF 约 0.4 MB 常驻 + 启用时 ≈0.27 MB。关闭语音时任务读完设置即退出，不占线程与堆。

### 9.3 没有板子验证不了的

1. `ny_voice_audio.c` 的 ioctl 序列在本板是否工作：录音 100 ms 小缓冲（ALLOCBUFFER 3200 B）、8 块缓冲对 `CONFIG_AUDIO_NUM_BUFFERS=2`、
   放音欠载时驱动行为（已用"缺数据时补 50 ms 静音"规避）、STOP 后是否一定收到 `AUDIO_MSG_COMPLETE`。
2. HALF 下麦克风关→放音开→放音关→麦克风开的切换耗时（决定提示音后丢多少话）。
3. SHARED_16K 的 16k 单声道放音、SHARED_44K 的 44.1k 立体声录音。
4. 门限默认值（RMS 150、×2.5）对真实麦克风增益是否合适；`voice.status.microphone.level` 可现场看。
5. 首音延迟、TTS RTF、三模型并发内存、真人唤醒率、自唤醒。
6. `agent.chat` 在 32 KB 栈上是否够（面板调用栈同量级，未测）。
7. 整固件链接（本次只做了 -fsyntax-only 与单文件 -c）。

### 9.4 上板逐环测试脚本（每步先过再下一步；全部**未上板**）

前置：固件与 nyampd 用同一版 `nyamp_protocol.h` / `rk3576_shmem_layout.h`；第 3 节文件放好；`/data/voice` 目录存在。
串口 NSH 执行。话题从 nsh 发：`echo '{"enabled":true}' > /tmp/v.json`，然后 `nycore product voice.enable /tmp/v.json`，结果以 `PRODUCT_RESULT {...}` 打印（下文简写成 `topic {json}`）。

| # | 命令 | 预期 |
|---|---|---|
| 0 | `nyampctl health` | `capabilities=0x0000007f`。`0x0f` = nyampd 没带语音后端，后面全部 `-ENOTSUP(-138)` |
| 1 | `nyampctl status` | running/linked、generation 非 0；`compute.status` 的 capabilities 含 `asr tts kws` |
| 2 | `nyampctl tts say "你好，我是星喵。" /data/voice/t.wav` | `tts loaded in ≈10000 ms`（首次含 155 MiB 拉取进度行）→ `window 0: … unit` … `last` → `finish: status 0, N windows`；文件为 44.1k 单声道。金标准句 `你好，我是星喵。忙了一天，辛苦啦。要不要休息一会儿？` 应为 4 窗 44100/44100/44100/42804、共 175104 采样 |
| 3 | `music.play {"name":…}` 或 nxplayer 播 `t.wav` | 听到人声 → TTS 数值正确，与播放线程无关 |
| 4 | `nyampctl tts say "你好，我是星喵。"` | 直接从喇叭出声，无爆音/断续 → `ny_voice_audio` 放音路径成立。`-EBUSY(-16)` = 喇叭被占或麦克风以别的格式开着 |
| 5 | `nyampctl tts say 稍等。 /data/voice/wait.wav`；同理 `not_heard.wav`(没听清，再说一遍吧。) `busy.wav` `error.wav` `approval.wav` | 预合成提示音就位（可选；没有则两声提示音） |
| 6 | 把 `test_wavs/0.wav` 放到 `/data/voice/0.wav`，`nyampctl asr file /data/voice/0.wav` | `asr loaded`（24 MiB，≈1–2 s）→ 多行 `partial … @<consumed>: <文本渐长>` → `final resync …: 对我做了介绍那么我想说的是呢大家如果对我的研究感兴趣呢` → `finish: status 0`。`1.wav` → `重点想谈个问题首先呢就是这一轮全球金融动量的表现` |
| 7 | `nyampctl asr mic 5` 然后说话 | `speak now, 5 s` → partial → final 为所说内容 → 录音路径（16k 单声道、100 ms 块）成立。`-EBUSY` = voice 还开着 |
| 8 | `nyampctl kws listen 30`，说"你好 openvela" | `keywords: nihao_openvela hello_openvela`、`grant: offset 0x120000 capacity 262144`；每秒一行 level（安静时应远小于说话时，用来校 `VAD_MIN_RMS`）；`DETECTED nihao_openvela id 0 flags 0x1 start … end … trigger …`，trigger−end 约 0.3–0.6 s；结束 `stream finished: status 0, N detections` |
| 9 | `voice.enable {"enabled":true}` → `voice.status {}` | `state:"idle"`、`models.kws.state:"ready"`、`streaming:true`、`microphone.open:true`；安静时 `gateOpen:false` 且 `windows` 不涨，出声后涨 |
| 10 | `voice.say {"text":"现在几点了，我来报时。"}` | 状态 speaking→idle，眼睛 happy→idle；HALF 下说话期间 `microphone.open:false`，说完恢复且 `windows` 继续涨 |
| 11 | `voice.listen {}` 后说"现在几点" | listening（眼睛 curious）→ 首次先"稍等"提示并加载 ASR → thinking（processing）→ speaking（happy）→ idle；`lastTranscript`/`lastReply` 有值；规则类回复 <1 s，聊天 3–30 s |
| 12 | 说"你好 openvela"，停顿后说"今天天气怎么样" | `wakeWord:"nihao_openvela"`、`detections`+1，其余同 11；一口气连说（不停顿）也应识别完整指令（attach 在 end_sample） |
| 13 | 唤醒后不说话 | ≈5 s 后"没听清"提示 → idle |
| 14 | 说一个需要批准的动作（如"把闹钟删掉"） | 说"这个操作需要你在面板上确认一下" → idle；面板批准后把结果说出来；10 分钟不批则放弃跟踪 |
| 15 | 回复播放中 `voice.cancel {}` / `voice.listen {}` | 立即静音；后者转 listening。SHARED+BARGE_IN 时用唤醒词也应能打断 |
| 16 | 思考或播放中在 Linux 侧重启 nyampd | `errors.text` 出现 stream lost，`models.*` 回 unloaded→kws 自动 ready，状态回 idle，不死锁；下一次唤醒正常（ASR/TTS 重新加载） |
| 17 | voice 开着时 `music.play`（**未打 9.1 的 claim 补丁**） | 预期 `-EBUSY`——这是已知缺口，不是回归；打补丁后应能播，播完唤醒恢复 |
| 18 | `voice.enable {"enabled":false}` → 重启 → `voice.status` | `enabled:false`、`running:false`、无 `ny_voice_*` 线程（`ps`） |
