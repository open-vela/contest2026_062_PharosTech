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

电话音频与 SCO 实验代码单独保留在本地 `organize/hold-bluetooth-20261017` 候选，
不随本轮产品提交。该候选仍须真手机联调。
