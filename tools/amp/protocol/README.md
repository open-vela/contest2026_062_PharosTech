# nyamp v1 控制协议

`nyamp` 是 openvela 控制域与 Linux 计算域之间的应用层协议。它运行在 RPMsg
endpoint上；本层定义版本、服务、请求关联、deadline、取消与generation字段。
已实现 HEALTH、LLM、BLOB（模型按需交付）与语音三服务 ASR/TTS/KWS；不提供独立重启隔离或权限代理。

所有整数固定为 little-endian，不能直接把 C struct 强转到线上。40 字节头布局：

| offset | 类型 | 字段 |
|---:|---|---|
| 0 | u32 | magic `NYAP` |
| 4 | u16 | version |
| 6 | u16 | header size |
| 8 | u16 | service |
| 10 | u16 | opcode |
| 12 | u32 | flags |
| 16 | u64 | request id |
| 24 | u64 | monotonic deadline（ms） |
| 32 | u32 | compute-domain generation |
| 36 | u32 | inline payload size |

RPMsg 可用 payload 是 496 字节，因此 v1 inline payload 最大 456 字节。图像、音频、
tensor、模型等大对象只能在 payload 中携带共享内存描述符，不得分片硬塞 RPMsg。

每条消息必须且只能是 request/response/event/cancel 之一；error 只允许附加在
response。`request_id=0`和`service=0`非法。Linux每次服务进程启动生成非零
generation；客户端检查当前请求ID与应答格式，不把它当成安全身份。

HEALTH opcode 0是READY事件，request_id=1、payload为空、generation非零。
服务端打开端点后主动发送，供OpenAMP学习Linux动态地址；客户端忽略其应用负载。
opcode 1查询健康状态，opcode 2返回Linux在线CPU与CPU part。

## 请求方向、request_id 与 generation（加入 BLOB 服务后的规则）

service 1–8 都是「控制域(openvela)请求、计算域(Linux)应答」。`NYAMP_SERVICE_BLOB = 9`
是第一个方向相反的服务：计算域没有存储，eMMC 与 `/data` 归控制域，所以**请求方是 Linux，
应答方是 openvela**。为此把原来隐含的三条规则写明，而不是另起一套机制：

- **request_id 归「发起方」所有，应答方只回显。** 两个域各有一个互不通信的分配器，
  因此单看 id 在端点上不唯一；用消息类型消歧——一个域收到的 RESPONSE/EVENT 只可能
  属于它自己发出的请求，收到的 REQUEST/CANCEL 只可能来自对端。计算域发起的 id 额外置
  最高位 `NYAMP_REQUEST_ID_COMPUTE`（bit 63）；这不是正确性所必需，而是让抓包无歧义，
  并让应答方能拒绝方向错误的请求。控制域的 id 是 `pid<<32 | 毫秒/计数`，永远到不了 bit 63。
- **generation 仍然只有一个，属于计算域。** 控制域没有自己的 generation：它从 READY
  事件（或 HEALTH 应答）学到当前值；BLOB 请求必须携带这个值，否则回
  `STALE_GENERATION`；应答回显请求里的 generation。学到新 generation 即视为 nyampd 重启，
  控制域丢弃全部已打开的 blob 与进行中的摘要计算。
- **永远不应答 RESPONSE/EVENT。** 现在两端都是应答方，对一个应答再回错误应答会让两端
  互相弹错误帧直到永远；这类帧一律丢弃（nyampd 的 `Dispatch` 返回 `response_size == 0`）。
- 共享内存仍然只有计算域一个分配器：READ 里的 NYBS 窗口由 Linux 铸造(lease =
  `generation<<32 | 计数`)，控制域只填充并原样回显，`length` 改成实际写入字节数。
  控制域只校验两件会破坏链路的事：窗口不得碰 arena 头(前 4 KiB)，不得越过 arena 末尾。
- OPEN 可能要在控制域算整文件 SHA-256（875 MB 约 12 s），请求方放弃时发一条
  `NYAMP_FLAG_CANCEL`、request_id 等于该 OPEN 的消息；否则 blob 会在无人 CLOSE 的情况下
  一直开到下一个 generation。

### BLOB opcode（应答 payload = `i32 status` + body）

| opcode | 方向 | 请求 body | 应答 body |
|---|---|---|---|
| 1 OPEN | Linux→openvela | `u32 flags(0)`, `u32 name_len`, name | `u32 blob_id`, `u32 flags`, `u64 size`, `u64 mtime`, `sha256[32]` |
| 2 READ | Linux→openvela | `u32 blob_id`, `u32 flags(0)`, `u64 file_offset`, NYBS(40) | 同布局；NYBS.`length` = 写入字节，`flags` bit0 = EOF |
| 3 CLOSE | Linux→openvela | `u32 blob_id`, `u32 0` | 无 |
| 4 LIST | Linux→openvela | `u32 cursor`, `u32 prefix_len`, prefix | `u32 next_cursor`, `u32 count`, 每项 `u64 size`,`u16 flags(bit0=目录)`,`u16 name_len`,name |
| 5 BENCH | Linux→openvela | `u32 mode`(0 回显/1 填窗), `u32 seed` [, NYBS] | 原样回显（填窗时 `length`=容量） |
| 0x10 BENCH_RUN | openvela→Linux | `u32 rounds`, `u32 window_bytes` | rounds, window, rtt min/avg/max(µs), fill/copy KiB/s, pattern_errors |
| 0x11 PULL | openvela→Linux | 同 OPEN（名字） | `u64 bytes`, `u64 elapsed_ms`, `u32 files`, `u32 reused` |
| 0x80 EVENT_PROGRESS | Linux→openvela | — | `u64 done`, `u64 total`, `u32 bytes_per_second`, `u32 0` |

名字规则（编码器与解码器各查一遍）：UTF-8、相对 `/data/models`、`/` 分隔，不得有空分量、
`.`、`..`、前导 `/`、反斜杠或控制字符，≤255 字节。OPEN 状态码：`NOT_READY`=不存在，
`UNSUPPORTED`=这是目录（请改用 LIST），`BUSY`=已有摘要在算或槽位用尽，`INVALID`=名字/窗口非法。
BENCH_RUN 与 PULL 走常规方向，存在的原因是计算域没有控制台：没有它们就只能靠「加载模型」
的副作用去验证交付通路。

## LLM CHAT（文本级补全，service 8）

GENERATE 收 token id，只适合自己持有分词器的调用方；控制域没有（分词器要 10 MB 词表和
逐位一致的 chat template，它们跟模型在一起）。CHAT 因此直接携带 OpenAI chat-completions
请求 JSON，渲染模板、分词、推理、解析工具调用全部在计算域完成。

| opcode | 方向 | body |
|---|---|---|
| 5 CHAT（请求） | openvela→Linux | `u32 total`, `u32 offset`, `u32 length`, `u32 max_new_tokens`, `u32 flags`, 然后 `length` 字节 JSON |
| 0x80 EVENT_TOKEN | Linux→openvela | 沿用原布局；**仅当** flags bit1 置位才发，文本保留 `<function` 等结构 token |
| 0x82 EVENT_RESULT | Linux→openvela | `u32 total`, `u32 offset`, `u32 length`, 然后 `length` 字节响应 JSON |
| 0x81 EVENT_FINISH | Linux→openvela | CHAT 专用 28 字节布局：`i32 status`, `u32 sequence`, `prompt_tokens`, `completion_tokens`, `prefill_ms`, `decode_ms`, `context_limit` |

- 请求体上限 64 KiB，分块最大 436 字节；各块共用一个 request_id、逐块应答，最后一块触发运行
  （与 GENERATE 同一思路）。参数每块都带、以首块为准，续块的 total/offset/参数/request_id 任一
  不符即丢弃整个半成品并回 `INVALID`。flags：bit0 `guard_untrusted`（请求数据里的 special
  token 字面量按普通文本处理，建议生产开启），bit1 `stream_tokens`。`max_new_tokens=0` 取
  守护进程默认值 256。
- 末块被接受后，同一 request_id 下依次：0..n 个 TOKEN（若请求）、RESULT 分块（仅 status=OK）、
  **恰好一个** FINISH。GENERATE 的 FINISH 仍是 8 字节；两种布局按所属请求的 opcode 区分，
  解码器互不接受对方的长度。
- 新状态 `NYAMP_MODEL_PROMPT_TOO_LONG = -11`：`prompt_tokens + max_new_tokens > context_limit`
  (2048)。它单列而不并入 INVALID，因为这是唯一期望调用方自行修复的失败：FINISH 带回
  `prompt_tokens` 与 `context_limit`，调用方据此裁剪历史后重试。模型此时不会被运行。
- 末块应答的状态：`NOT_READY`=没有加载模型（调用方可先 LOAD 再重试），`UNSUPPORTED`=已加载的
  模型没有 tokenizer（绝对路径加载裸 .rkllm）或守护进程无后端，`BUSY`=已有运行在途。
- 取消沿用 GENERATE：opcode CANCEL、request_id = 该 CHAT 的 id；FINISH 报 `CANCELLED`，不发 RESULT。
- 响应 JSON 是标准 `chat.completion`：`choices[0].message{role,content|null,tool_calls[{id,type:
  "function",function{name,arguments(JSON 字符串)}}]}`、`finish_reason` = `stop`/`tool_calls`/
  `length`、`usage`。解析不完整的工具调用原样留在 content，绝不执行半条命令。
- HEALTH capability bit3 = 守护进程能服务 CHAT。
- LOAD 逻辑名时一并拉取并加载同目录的 `tokenizer.json`（文件名 `llm/model.rkllm` →
  `llm/tokenizer.json`；目录名则取目录内的）。缺失即 LOAD 失败 `NOT_READY`，且在搬动模型之前
  就失败。stop id = `[1, 130073, 130072]`；BOS 只由模板输出一次。

## 语音：ASR(3) / TTS(4) / KWS(10)，SPEAKER(11) 仅占号

三个服务都是「控制域请求、计算域应答」。LOAD 的 payload 是模型目录：绝对路径原样交给后端，
逻辑目录名 `asr` / `tts` / `kws` 先经 BLOB 逐文件拉到 `/tmp/models/<名字>` 再加载，应答延后、
期间以 LOAD 的 request_id 发 `BLOB/EVENT_PROGRESS`（与 LLM LOAD 完全一致）。
HEALTH capability：bit4 = ASR、bit5 = TTS、bit6 = KWS；这三位表示**后端真的编进来了**，
没有对应运行时的 nyampd 照常启动，这三位为 0，相应 opcode 一律回 `UNSUPPORTED`，LLM/BLOB 不受影响。
同一请求的所有消息（BEGIN/PUSH/END/RELEASE/CANCEL、SYNTH_TEXT 各分块/RELEASE/CANCEL）与它的事件
都用 BEGIN（或首块）的 request_id。取消两种写法等价：CANCEL opcode（有应答），或
`NYAMP_FLAG_CANCEL` 帧（payload 为空、request_id 指目标、**无应答**，给不打算再读应答的请求方用）。
UNLOAD 会结束进行中的请求，但不让传输循环等推理：此时回 `BUSY`，见到该请求的 FINISH 后重发即 `OK`。

### 共享内存槽位契约（`chips/rk3576/include/rk3576_shmem_layout.h`）

没有采用「一个 1 MiB 槽半双工」：唤醒词流永不停止，而 `NYAMP_SLOT_SHARED` 同时是每次模型拉取的窗口，
一句合成语音又会占满它——共用就意味着每次拉模型、每句播报期间机器人都是聋的。arena 原有 2.9 MiB
未分配，所以改为**精确切分**，控制域无需改布局常量，只按 grant 里的 offset 写：

| 槽 | 偏移 / 大小 | 方向 | 属主（同一时刻只有一个） | 互斥手段 |
|---|---|---|---|---|
| `NYAMP_SLOT_SHARED` | `0x001000` / 1 MiB | 模型拉取：控制→计算；TTS PCM：计算→控制 | 一次 BLOB 拉取，**或**一个 TTS 请求 | BlobClient 的属主标志（同一把）：拉取进行中 SYNTH_TEXT 回 `BUSY`，合成进行中 PULL/逻辑名 LOAD 回 `BUSY` |
| `NYAMP_SLOT_CAPTURE` | `0x120000` / 256 KiB（4 个 1 s float32 窗） | 采集音频：控制→计算 | KWS 流，**或**一个 PUSH 方式的 ASR 请求 | 属主标志：另一个 BEGIN 回 `BUSY`；KWS 流运行时 ASR 用 attach 方式，不占槽 |

lease 由计算域单边铸造：`generation<<32 | bit31 | 计数`（bit31 把语音 lease 与 BLOB 的裸计数 lease 分开）。
每个 grant、每个 TTS 窗口各有独立 lease；lease/generation 不符、范围越出 grant、不是整数个采样、
采样格式与 BEGIN 不符的窗口一律 `INVALID` 且不读其内容。
「说话时闭麦」不再是内存安全问题，而是控制域的防自唤醒策略：SPEAKING 期间停止 KWS_PUSH 即可，
恢复时位置照实继续计数（或置 `DISCONTINUITY`），计算域据此重置解码器——见下。

### ASR（service 3）

| opcode | body | 应答 body |
|---|---|---|
| 1 LOAD | 目录名 | 无（逻辑名时延后） |
| 2 UNLOAD | 无 | 无 |
| 3 BEGIN | `u32 sample_rate(16000)`, `u16 channels(1)`, `u16 flags`, `u32 max_samples`, `u32 0`；attach 形式再加 `u64 start_sample`（共 24 B） | PUSH 形式：NYBS(40)，覆盖整个 CAPTURE 槽；attach 形式：无 |
| 4 PUSH | NYBS(40) + `u32 sequence`, `u16 flags(0)`, `u16 0`, `u32 total_samples`, `u32 consumed_samples` | 无 |
| 5 RELEASE | BEGIN 返回的 NYBS | 无 |
| 6 CANCEL | 无 | 无 |
| 7 END | `u64 end_sample`（`0xFFFF_FFFF_FFFF_FFFF` = 到目前为止的全部） | 无 |
| 0x80 EVENT_PARTIAL | `u32 sequence`, `u32 consumed_samples`, `u16 flags`, `u16 0`, UTF-8 文本 | |
| 0x81 EVENT_FINISH | `i32 status`, `u32 sequence`(= 已发 PARTIAL 帧数) | |

- BEGIN flags：bit0 `S16`（窗口是 int16，否则 float32）；bit1 `ATTACH_KWS`（24 B 形式必须置位，16 B 形式必须清零）。
  `max_samples`=0 取 60 s，上限 60 s。
- **PUSH 方式**：grant 在整个请求期间有效。每个 PUSH 指 grant 内的一个子范围（同 lease），
  **PUSH 的应答发出时采样已被拷走**（先拷贝、后解码），该范围立即可重写——控制域可用一个窗口，
  也可两个窗口乒乓，不需要逐窗口 RELEASE。`sequence` 必须从 0 连续，跳号 = 丢窗 = `INVALID`。
  累计超过 `max_samples` 的 PUSH 回 `INVALID`（请求继续，可 END）。
- **结束由控制域决定**：END，或最后一个 PUSH 的 NYBS 带 `NYAMP_BUFFER_LAST`（长度可为 0）。
  之后服务冲刷解码器尾部，发最终文本（`FINAL`）与 FINISH(OK)。重复 END 无害；
  PUSH 方式的 END 只接受「到目前为止」，给具体位置回 `INVALID`。
- **attach 方式**：不推音频，而是从 `start_sample`（通常取 DETECTED 的 `end_sample`）起读 KWS 流的环形缓冲。
  无 grant、PUSH 回 `INVALID`。`start_sample` 早于环里最旧的采样回 `INVALID`（不会悄悄从后面开始）；
  晚于当前位置则等待。END 给出结束位置（可在未来）；累计读满 `max_samples` 自动结束；
  KWS 流结束（END/UNLOAD/CANCEL）时请求以已读到的音频正常结束。没有 KWS 流时 BEGIN 回 `NOT_READY`。
- **EVENT_PARTIAL**：接收方维护一个字符串，`RESYNC`(bit0) = 用本帧文本替换，否则追加。只增长时发后缀；
  transducer 改写了前面的字就整段 RESYNC；一帧装不下（>444 B）= RESYNC 帧 + 追加帧，按 UTF-8 边界切。
  `ENDPOINT`(bit1) = 解码器认为说完了（sherpa 规则：有语音后静音 1.2 s / 无语音 2.4 s / 满 20 s），
  只在**上升沿**发一次、可以是空文本帧，**仅供参考**，服务不会因此结束请求。
  `FINAL`(bit2) = 本请求最后一帧文本，最终文本总是 RESYNC+FINAL，取消/超时的请求没有 FINAL。
  PARTIAL 是建议性的：事件队列满（64）时丢弃，丢弃后的下一帧必为 RESYNC；FINAL 与 FINISH 永不丢。
- 状态：`NOT_READY` 未 LOAD / 请求 id 不对，`BUSY` 已有请求在途或 CAPTURE 槽被占，
  `UNSUPPORTED` 无后端或共享区不可映射，FINISH 里 `CANCELLED` / `DEADLINE`（头部 deadline_ms 到期）。

### TTS（service 4）

| opcode | body |
|---|---|
| 1 LOAD / 2 UNLOAD | 目录名 / 无 |
| 3 SYNTH | （旧：音素 id）**恒回 `UNSUPPORTED`**——控制域没有 G2P，造不出这些 id |
| 4 RELEASE | 原样回显 EVENT_PCM 里的 NYBS |
| 5 CANCEL | 无 |
| 6 SYNTH_TEXT | `u32 total`, `u32 offset`, `u32 length`, `u32 speaker_id`, `f32 speed`, `u32 window_samples`, `u32 flags(0)`，然后 `length` 字节 UTF-8 |
| 0x80 EVENT_PCM | NYBS(40) + `u32 sequence`, `u32 sample_rate(44100)`, `u32 channels(1)`, `u32 valid_samples` |
| 0x81 EVENT_FINISH | `i32 status`, `u32 sequence`(= PCM 事件数), `u32 total_samples` |

- 文本 ≤16384 B，分块 ≤428 B，各块共用 request_id、逐块应答、末块触发合成（与 CHAT 同一套分块规则：
  参数每块都带、以首块为准，续块的 total/offset/参数/request_id 任一不符即丢弃半成品并回 `INVALID`；
  offset=0 的块总是重新开始）。分块可以切在 UTF-8 序列中间，收齐后整体校验，非法 UTF-8 / 含 NUL 回 `INVALID`。
  `speed` 0.5–2.0；`window_samples` 0 = 44100（1 s），范围 4410–262144（整个槽）。
- **文本前端在计算域**（`tools/amp/g2p`：归一化、分词、G2P、分句），因为 7 MB 词典跟着模型走。
- **永不截断**：声码器只有一个 512 帧桶（≈5.94 s）。前端按 56 符号预算分句/分子句/必要时按词切
  （语速 <1 时预算同比例缩小）；前缀模型实际给出的帧数才是权威——仍超 512 帧的单元用一半预算**再切**后重试；
  切不动的文本使请求以 `UNSUPPORTED` 结束（已能说的部分照常送出，绝不送被截短的窗口）。
  没有可发音内容的文本 = FINISH(OK)、0 个窗口。
- **PCM 窗口**：float32、单声道、44100 Hz，`valid_samples` 精确（单元总和 = 帧数×512），
  `NYBS.length = valid_samples×4`，之后的字节无意义。NYBS flags：`FROM_COMPUTE`；`RESYNC` = 新单元（句边界）的首窗；
  `LAST` = 整个请求的最后一窗。**同一时刻只有一个窗口在外**：服务写窗 → 发 EVENT_PCM → 等到回显同一 lease 的
  RELEASE 才再碰槽。迟到的旧窗口回显、伪造 lease、别的 request_id 的 RELEASE 一律 `INVALID`，不会释放当前窗口。
  30 s 无人 RELEASE 则请求以 `DEADLINE` 结束并归还槽。
- **取消**粒度：单元之间、前缀/声码器两段之间、等待 RELEASE 期间。CANCEL 之后在外的窗口作废、不得再读，
  FINISH(`CANCELLED`) 立即到达，其 `sequence` 为已发窗口数。对只收了一半文本的请求 CANCEL = 丢弃半成品。

### KWS（service 10）——常驻唤醒词流，采集音频的属主

| opcode | body | 应答 body |
|---|---|---|
| 1 LOAD | `f32 threshold`, `f32 score`, `u16 max_active_paths`, `u16 num_trailing_blanks`, `u16 dir_len`, `u16 keywords_len`, 目录名, 关键词文件名 | 无 |
| 2 UNLOAD | 无 | 无 |
| 3 BEGIN | `u32 16000`, `u16 1`, `u16 flags`(bit0 S16), `u32 window_samples`(1600–16000，0=16000), `u32 0` | NYBS(40)，覆盖整个 CAPTURE 槽 |
| 4 PUSH | NYBS(40) + `u32 sequence`, `u16 flags`(bit0 DISCONTINUITY), `u16 0`, `u64 stream_sample` | `u64` 期望的下一个 stream_sample |
| 6 END | 无 | 无 |
| 7 LIST | 无 | `u16 count`, `u16 0`, 每个 label `u16 len` + UTF-8 |
| 0x80 EVENT_DETECTED | `u32 sequence`, `u16 keyword_id`, `u16 flags`, `f32 score`, `u32 label_len`, `u64 start_sample`, `u64 end_sample`, `u64 trigger_sample`, label | |
| 0x81 EVENT_FINISH | `i32 status`, `u32 sequence`(= DETECTED 数) | |

- LOAD 的数值参数为 0 = 取 `tools/amp/voice/README.md` 评估出的默认值（0.10 / 2.0 / 16 / 1）。关键词文件名为空 =
  `keywords.txt`，且必须是目录内的单个路径分量。参数不同算另一个模型：相同参数重复 LOAD = `OK`，不同 = `BUSY`（先 UNLOAD）。
  keyword id = label 在关键词文件中首次出现的序号（LIST 可查），发音变体共享 id。
- `stream_sample` = 自 BEGIN 起的 u64 绝对采样计数。PUSH 带首采样位置；位置不是期望值、`sequence` 跳号或置了
  `DISCONTINUITY` 都**不是错误**：丢弃环内容与解码器状态，从新位置继续计数（未读完的旧采样随之丢弃——真实的暂停以秒计，无影响）。
  位置**倒退**回 `INVALID`（否则已报告过的偏移全部产生歧义）。窗口 1..window_samples 个采样，应答同样在拷贝后、解码前发出。
- 计算域在自己的内存里保留流的最后 `NYAMP_KWS_RING_SECONDS`=10 s。环归 KWS 流所有：单写者（传输线程）、
  读者各持游标互不消费（唤醒词解码线程、attach 的 ASR），流结束时读者看到「已关闭」而不是悬空指针。
  因为所有权这样简单且可单测（`nyampd_audio_test`），才采用了「ASR 从偏移 N 接入 KWS 流」；PUSH 方式保留给没有 KWS 流的场合。
- DETECTED flags：bit0 `HAS_OFFSETS`、bit1 `HAS_SCORE`（sherpa 1.13.8 不导出分数，恒 0）。`trigger_sample` 恒有效
  （服务自己的计数）。`start/end_sample` 来自解码器的 token 时间戳，主机实测发现长流里它偶尔指向很久以前的 token
  （一次真实触发，「短语」却在一分钟前），因此服务只在偏移可信时才置 `HAS_OFFSETS`：`start<end≤trigger`、
  短语 ≤6 s、`trigger-end` ≤2 s；否则偏移清零，控制域改用 `trigger_sample`（例如从 `trigger-0.5 s` 接入 ASR）。
- 事件队列满（32）时丢 DETECTED（只丢这一个，流不停）；FINISH 不丢。一个流、一个控制域：END 与 UNLOAD
  不看 request_id 就结束当前流（重启过的控制域任务没有别的办法要回槽）；`NYAMP_FLAG_CANCEL` 帧须指 BEGIN 的 id，FINISH 报 `CANCELLED`。
  END 后 BEGIN 要等 FINISH（此前回 `BUSY`）。
- SPEAKER（11）只保留了服务号；`tools/amp/voice` 的声纹库与 `PROTOCOL.md` 第 4 节的提案未接入，任何 opcode 回 `UNSUPPORTED`。

主机测试：

```sh
cmake -S tools/amp/protocol -B out/nyamp-protocol
cmake --build out/nyamp-protocol
ctest --test-dir out/nyamp-protocol --output-on-failure
```

该层只验证消息形状；插件权限、独立重启的生命周期不在本层。共享大块数据的描述符(NYBS)
与 BLOB 窗口在本层编解码，放置策略与校验在两端服务里。
