# 依赖树补丁

`app/nyabula_core` 依赖上游 `packages/ai_agent` 与 `apps/system/nxplayer` 尚未合入的接口；
在纯 manifest 检出上直接编 `configs/product` 会在 `ny_agent.c` / `ny_product_media.c` 报
未声明符号。`tools/setup_workspace.sh`（`make setup`）会自动、幂等地应用本目录补丁。

| 补丁 | 目标 | 内容 |
|---|---|---|
| `packages-ai_agent-nyabot.patch` | `packages/ai_agent`（openvela.xml `packages_ai_agent`，2026-06-30 基线） | Nyabot 适配：`tool_registry_init_empty`、`context_set_provider`、`agent_loop_status`、`agent_msg_t.interim/request_id`、`http_response.c`、Feishu/WeChat/MQTT/Node 通道改造、skills loader（对应 nyabot-*-20260912 系列） |
| `packages-ai_agent-on-device-llm.patch` | `packages/ai_agent`（在 nyabot 补丁之后应用，按文件名排序自然满足） | `llm_set_local_transport()`：为某个后端 host 注册本机传输，路由到该 host 的请求原样交给回调而不走 HTTPS；端侧模型因此复用云端后端的路由、重试、工具解析与用量统计（`app/nyabula_core/ny_agent_local.c`） |
| `apps-nxplayer.patch` | `apps/system/nxplayer` + `apps/include/system/nxplayer.h` | `NXPLAYER_STATE_*`、`nxplayer_getstate()`、`nxplayer_playpcmfd()` |

手动应用：

```sh
patch -d packages/ai_agent -p1 -N < contest2026_062_PharosTech/patches/packages-ai_agent-nyabot.patch
patch -d apps              -p1 -N < contest2026_062_PharosTech/patches/apps-nxplayer.patch
```

另外 `app/nyabula/src/generated/fonts/nyabula_font_*.c` 被 gitignore（约 5.7 MB 生成物），
链接前需用 `app/nyabula/tools/generate_fonts.py` 生成或从上次构建复制。
