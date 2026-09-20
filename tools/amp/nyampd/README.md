# nyampd

`nyampd` 是 Linux AMP 计算域唯一的控制入口。它直接读写由
`rockchip_rpmsg_mbox` 和 `rpmsg_char` 创建的消息型字符设备；默认从sysfs寻找名字为
`rpmsg-raw`的endpoint对应的`/dev/rpmsgN`，不依赖动态编号。收到畸形包时丢弃，
transport EOF/错误或短写时立即退出，让 PID1 重新拉起并生成新的 generation。

打开端点后先发送READY事件完成地址发现；已实现 health/info、LLM、模型交付(BLOB) 与语音三服务
ASR/TTS/KWS。NPU、ISP 必须在各自 vendor runtime 和 buffer 生命周期确定后作为独立提交加入，不能先返回伪成功。

语音服务（线格式与槽位契约见 `../protocol/README.md`「语音」一节，计划与板测步骤见 `docs/voice-chain-plan.md`）：
`nyampd_asr.*`（流式识别：边收边解码，增量文本事件，上报 sherpa 的 endpoint 标志；PUSH 或 attach 到 KWS 流）、
`nyampd_tts.*`（UTF-8 文本 → `tools/amp/g2p` → 逐单元合成 → PCM 窗口，一次只有一个窗口在外，永不截断）、
`nyampd_kws.*`（常驻唤醒词流 + 10 s 环形缓冲，`CaptureRing` 在 `nyampd_audio.*`）、`nyampd_loader.*`
（三者共用的 LOAD：绝对路径同步加载，逻辑目录名 `asr`/`tts`/`kws` 经 BLOB 拉取后加载）。
每个服务照 `nyampd_llm` 的骨架：单会话 + worker 线程 + 有界事件队列 + 主循环泵出；传输线程上的入口都不等推理。
后端都在小接口后面（`models::AsrStreamBackend`、`KwsBackend`、`TtsFrontend` + `models::Backend`），
真实实现只在给了运行时才编进来：`NYAMP_SHERPA_INCLUDE/LIBRARY` → ASR + KWS（`nyampd_kws_sherpa.cpp`
包 `tools/amp/voice`），`NYAMP_ORT_*` + `NYAMP_RKNN_*` → MeloTTS（仅 aarch64）。都不给时照常构建运行，
三个服务回 `UNSUPPORTED`，HEALTH capability bit4/5/6 为 0，`info` 多一行 `asr=<状态>:<上次LOAD线上状态> tts=… kws=…`
（状态 none/off/loading/ready/busy|listening）。
单测 `nyampd_audio_test`、`nyampd_asr_test`、`nyampd_tts_test`、`nyampd_kws_test`（脚本后端；链接 sherpa 并设
`NYAMP_ASR_MODEL`+`NYAMP_ASR_WAVS`、`NYAMP_KWS_MODEL`+`NYAMP_KWS_POSITIVE`+`NYAMP_KWS_NEGATIVE`、
`NYAMP_G2P_ASSETS`[+`NYAMP_G2P_GOLDEN`] 时加跑真模型/真前端），线上端到端 `tools/amp/test_voice_flow.py`
（客户端是纯 Python 按协议文档手工打包，与服务端不共享任何编解码代码）。
aarch64：`build_arm64.sh` 读同名环境变量，链接了任一动态运行时就加 `-DCMAKE_SKIP_RPATH=ON`。

模型交付：计算域没有存储，模型在 openvela 的 `/data/models`。`BlobClient`(nyampd_blob.*)
经 `NYAMP_SERVICE_BLOB` 以 1 MiB 共享内存窗口把命名 blob 拉到 tmpfs
（默认 `/tmp/models/<name>`，可用环境变量 `NYAMPD_MODEL_ROOT` 改），增量 SHA-256 与 OPEN
给出的摘要、大小核对后才落成正式文件并写 `<file>.sha256` 标记；标记与大小都吻合则直接复用。
`ModelProvisioner`(nyampd_provision.*) 供 LLM LOAD 使用：相对的逻辑名（文件或目录）先补齐
缺失文件再把 tmpfs 路径交给后端，绝对路径原样直通。拉取在 worker 线程进行，主循环继续收发
（它正是投递 BLOB 应答的那个循环），LOAD 的应答延后经服务队列发出；期间以 LOAD 的
request_id 发 `BLOB/EVENT_PROGRESS`。`BlobService` 处理 openvela 发来的 `BENCH_RUN`
（rpmsg 往返时延 + 共享窗口吞吐，带位置相关图样校验）与 `PULL`（只拉不加载）。
RESPONSE/EVENT 帧永不被应答。单测 `nyampd_blob_test` 用进程内的 openvela 应答器验证。

本地 LLM 的文本级接口：`NYAMP_LLM_CHAT`（nyampd_chat.*、nyampd_llm.* 的 ChatWorker）链接
`tools/amp/chat` 库（`add_subdirectory(../chat chat)`，该库仍可独立构建）。流程：分块收齐请求
JSON → chat template 渲染 + 分词（`guard_untrusted` 可选）→ `prompt+max_new > 2048` 则以
`PROMPT_TOO_LONG` 拒绝 → 走与 GENERATE 相同的 token-id 运行路径 → 收集输出 **token id**
（RKLLM 回调本来就给 `token_id`，且后端 `skip_special_token=false`，后端无需改动）→ 用本库
`decode(skip_special=false)` → 按请求里的工具 schema 解析 → `EVENT_RESULT` 分块 → `EVENT_FINISH`
带统计。遇到 stop id `[1,130073,130072]` 即结束，stop token 不计入回答。逻辑名 LOAD 会连同
`tokenizer.json` 一起拉取加载；绝对路径 LOAD 行为不变（旁边恰有 tokenizer.json 就顺带启用 chat，
否则 CHAT 回 unsupported）。顺带修了两处旧问题：Cancel 不再去拿 worker 整个运行期间持有的
session 锁（原先 cancel 会把传输循环卡到运行结束）；Session 要求 request id 单调递增，而线上 id
含发起任务的 pid、并不单调，现改为服务内部自增的 run id。发送遇到 EAGAIN/ENOMEM（发送环暂满，
chat 结果是一串突发帧）改为保留该帧下轮重发，不再退出守护进程。
单测 `nyampd_chat_test`（字节 codec + 脚本后端；设环境变量 `NYAMP_TOKENIZER_JSON` 可加跑真词表项），
端到端 `tools/amp/test_chat_flow.py`（真实 nyampctl 客户端 ↔ 真实服务，SOCK_SEQPACKET）。

deadline 使用两端共享的 ARM generic counter 换算毫秒。AArch64 生产构建读取
`cntvct_el0/cntfrq_el0`；主机单测显式注入当前值，不依赖主机时钟。

主机构建与测试：

```sh
cmake -S tools/amp/nyampd -B out/nyampd
cmake --build out/nyampd
ctest --test-dir out/nyampd --output-on-failure
```

运行：

```sh
/usr/sbin/nyampd
```

也可在诊断时显式传入设备路径。openvela侧`nyampctl health`首次运行会创建并公告
`rpmsg-raw` channel，initramfs supervisor会在endpoint出现后自动拉起daemon。
