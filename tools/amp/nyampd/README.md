# nyampd

`nyampd` 是 Linux AMP 计算域唯一的控制入口。它直接读写由
`rockchip_rpmsg_mbox` 和 `rpmsg_char` 创建的消息型字符设备；默认从sysfs寻找名字为
`rpmsg-raw`的endpoint对应的`/dev/rpmsgN`，不依赖动态编号。收到畸形包时丢弃，
transport EOF/错误或短写时立即退出，让 PID1 重新拉起并生成新的 generation。

打开端点后先发送READY事件完成地址发现；已实现 health/info、LLM 与模型交付(BLOB)。NPU、ISP、ASR/TTS必须在各自
vendor runtime 和 buffer 生命周期确定后作为独立提交加入，不能先返回伪成功。

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
