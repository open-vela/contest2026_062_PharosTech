# 依赖树补丁

`app/nyabula_core` 依赖上游 `packages/ai_agent` 与 `apps/system/nxplayer` 尚未合入的接口；
在纯 manifest 检出上直接编 `configs/product` 会在 `ny_agent.c` / `ny_product_media.c` 报
未声明符号。`tools/setup_workspace.sh`（`make setup`）会自动、幂等地应用本目录补丁。

| 补丁 | 目标 | 内容 |
|---|---|---|
| `packages-ai_agent-nyabot.patch` | `packages/ai_agent`（openvela.xml `packages_ai_agent`，2026-06-30 基线） | Nyabot 适配：`tool_registry_init_empty`、`context_set_provider`、`agent_loop_status`、`agent_msg_t.interim/request_id`、`http_response.c`、Feishu/WeChat/MQTT/Node 通道改造、skills loader（对应 nyabot-*-20260912 系列） |
| `packages-ai_agent-on-device-llm.patch` | `packages/ai_agent`（在 nyabot 补丁之后应用，按文件名排序自然满足） | `llm_set_local_transport()`：为某个后端 host 注册本机传输，路由到该 host 的请求原样交给回调而不走 HTTPS；端侧模型因此复用云端后端的路由、重试、工具解析与用量统计（`app/nyabula_core/ny_agent_local.c`） |
| `packages-ai_agent-tls-chunked-end.patch` | `packages/ai_agent/src/infra/vela_tls.c` | chunked 响应在终止块处结束读取。原代码对没有 Content-Length 的响应一直读到服务器关闭 keep-alive 连接（约 90 s），超过 Agent 60 s 看门狗，完整的回答被当成超时丢弃 |
| `apps-nxplayer.patch` | `apps/system/nxplayer` + `apps/include/system/nxplayer.h` | `NXPLAYER_STATE_*`、`nxplayer_getstate()`、`nxplayer_playpcmfd()` |
| `apps-microadb-forward-close.patch` | `apps/system/adb/microADB/tcp_service.c` | 修复 `adb forward` 流的拆除：连接尚未完成（或失败）时主机关流，原代码直接 free 服务结构却不 `uv_close()` 已注册的 TCP 句柄，随后 `connect_cb` 从已释放内存取函数指针 → adbd 任务 panic、整机复位。改为任何状态都经 `uv_close()` 释放，并防重入 |

手动应用：

```sh
patch -d packages/ai_agent -p1 -N < contest2026_062_PharosTech/patches/packages-ai_agent-nyabot.patch
patch -d apps              -p1 -N < contest2026_062_PharosTech/patches/apps-nxplayer.patch
```

另外 `app/nyabula/src/generated/fonts/nyabula_font_*.c` 被 gitignore（约 5.7 MB 生成物），
链接前需用 `app/nyabula/tools/generate_fonts.py` 生成或从上次构建复制。

## 本地整理新增依赖修复（2026-10-17）

- `apps-wamr-context-default.patch`：关闭 module instance context 时显式定义为 0，
  修复 CMake 的 `-Werror=undef`；不改变开启该功能时的值。
- `packages-ai_agent-slash-remind-bounds.patch`：提醒回复按容量追加文本，保留原有
  截断行为，消除 CMake 的 `-Werror=format-truncation`，不全局关闭警告。

使用 `bash tools/apply_dependency_patches.sh <openvela工作区>`，脚本会识别已应用
的重叠补丁栈，真正冲突则非零退出。原始依赖源码副本与版本记录在本地整理报告中。

- `nuttx-audio-fake-stop.patch`：仅 sim 的假音频驱动需要，序列化队列关闭与 STOP；
  `core_services` CI 显式应用。普通硬件 ES8388 路径不受影响。

电话音频候选已于 2026-10-18 按用户要求接入本地 integration，启用
A2DP Sink / AVRCP / HFP HF / SCO 与持久化配对。当前完成宿主测试和
产品 AMP 完整链接；真实手机和 USB 2.1 仍待板测，不据此合入远端。

| 新增补丁 | 应用目录 | 修复 |
|---|---|---|
| `nuttx-bluetooth-sco.patch` | `nuttx` | H4 SCO 包头与上下行类型 |
| `external-zblue-h4-build.patch` | `apps/external/zblue` | Make/CMake 纳入 H4 传输驱动 |
| `zblue-h4-thread-width.patch` | `apps/external/zblue/zblue` | H4 启动保留指针宽度的线程返回值 |
| `zblue-key-aging-controller.patch` | 同上 | 配对密钥使用记录传递所属控制器 |
| `zblue-delayed-settings-controller.patch` | 同上 | GATT 延迟持久化使用正确的控制器和工作队列上下文 |

`apply_dependency_patches.sh` 已纳入上述补丁，并保持冲突报错、已应用幂等。


- `packages-ai_agent-zz-mimo.patch`：仅向 MiMo 官方主机的普通对话与工具对话
  请求添加 `thinking.type`；`none` 对应 disabled，其余偏好对应 enabled。
  其他服务商请求不变。按文件名最后应用，保留现有工具推理历史处理。

- `apps-libwebsockets-cloud.patch`：在 manifest 自带的 openvela libwebsockets
  移植中，仅对服务端 ALPN 初始化加 `LWS_WITH_SERVER` 条件，允许客户端专用
  配置编译。`lws_tls_server_conn_alpn` 同样被客户端使用，保持可编译。
  应用前须通过 manifest 同步 `apps/netutils/libwebsockets/libwebsockets`；
  不要用未移植的 upstream tar 包替代。构建隔离副本时保留
  `lib/core-net/output.c`，它含必需的发送函数。

- `zblue-port-init-api.patch`：公开宿主显式调用的z_sys_init；产品使用pthread_once初始化。
- `zblue-nuttx-smp-config.patch`：保留宿主CONFIG_SMP，NuttX端口锁使用全局可嵌套临界区，避免按UP布局内联访问SMP调度结构。4核产品编译通过；硬件结果见实测日志。

- `nuttx-usb-audio-packet-boundary.patch`：USB播放按采样率和服务周期组包，
  仅在工作区已有USB Audio类驱动时应用，不要求非USB工作区新增该驱动。
  将应用缓冲尾部保存在驱动自己的DMA缓冲中，与下一已排队缓冲拼接；
  只有EOF或队列确实耗尽时才补齐最后一个USB包。避免2.1的1024字节缓冲
  被拆成五个192字节包加一个64字节短包，拖慢低音和板载输出。
  宿主回归：`python3 tools/usbhost_audio_packet_test.py WORKSPACE/nuttx/drivers/usbhost/usbhost_audio.c`，
  覆盖48kHz、44.1kHz、不同应用块长、末尾补零、短传输/错误及Mic变长包。

- `packages-ai_agent-zz-mimo-model-default.patch`：MiMo新建主对话配置默认改用
  `mimo-v2.6-flash`，替换已退役的v2 Flash；不重写已保存的主机、密钥或模型选择。
  图片理解的旧2.5配置由产品适配层兼容迁移；ASR/TTS仍保留官方2.5模型ID。

- `packages-ai_agent-zzz-05-tool-output.patch`：工具结果缓冲区可配置，默认仍为
  8 KiB（并行默认16 KiB）；产品设32 KiB，容纳合法8 KiB源码的JSON转义及24 KiB
  UI文档。修复实机7979字节源码被inspect序列化为8202字节后返回E2BIG的问题。

- `packages-ai_agent-zzz-06-cascade-tools.patch`：AUTO 路由升级到高质量后端后，
  其响应重新进入公共的统计与派发路径。带正文或无正文的 tool_calls 都不会被当成
  最终文字吞掉；每轮只升级一次，普通文本、失败回退和无可升级后端的路径保留。
  回归：`python3 tools/nyabula_core/tests/agent_cascade_approval_sim.py SIM_BINARY --output OUTPUT_DIR`。
  覆盖批准/拒绝/取消、只读调用、纯文本、失败回退，以及纯口头承诺不产生虚假审批。

- `packages-ai_agent-zzz-07-tool-trace.patch`：增加协议级计数，记录实际发出的工具数量、
  响应中 tool_calls 数量、结构化解析数量、最终调用数量和结束标志。新 Tool trace 行
  不包含提示词、参数、返回内容、思考文本或密钥。`wire=-1` 表示该字段不是数组。
  用于区分“模型未发调用”与“解析/派发路径丢失”，不能将自然语言承诺当作执行证据。
