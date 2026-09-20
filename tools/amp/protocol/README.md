# nyamp v1 控制协议

`nyamp` 是 openvela 控制域与 Linux 计算域之间的应用层协议。它运行在 RPMsg
endpoint上；本层定义版本、服务、请求关联、deadline、取消与generation字段。
已实现 HEALTH、LLM 与 BLOB（模型按需交付）；不提供独立重启隔离或权限代理。

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

主机测试：

```sh
cmake -S tools/amp/protocol -B out/nyamp-protocol
cmake --build out/nyamp-protocol
ctest --test-dir out/nyamp-protocol --output-on-failure
```

该层只验证消息形状；插件权限、独立重启的生命周期不在本层。共享大块数据的描述符(NYBS)
与 BLOB 窗口在本层编解码，放置策略与校验在两端服务里。
