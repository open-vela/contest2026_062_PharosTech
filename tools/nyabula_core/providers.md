# Nyabula Core UI/AI Provider 接口

`ny_provider_register()`把设备服务适配器接到 Core Broker。QuickJS 与 WAMR
不会直接访问显示、AI 服务或传输层；两种运行时都只调用 Broker，由 Broker
完成权限检查后转交 provider。

```c
static const struct ny_provider_ops_s g_ops =
{
  .ui_notify = board_ui_notify,
  .ai_invoke = board_ai_invoke,
};

ret = ny_provider_register(&g_ops, service_context);
```

注册应在启动任何插件前完成，provider 及其 context 必须在 Core 进程整个生命
周期内有效。当前不支持运行时替换。回调可能由不同插件 worker 调用，设备适配
器负责把请求投递到自己的服务线程或串行队列。

`ui_notify`接收 Core 已识别的插件 ID 和有界消息。`ai_invoke`接收 prompt，向
调用者提供的缓冲区写入响应并返回实际字节数；响应不得超过
`CONFIG_NYABULA_CORE_AI_RESPONSE_LIMIT`。返回负 errno 表示失败。

sim 的 `CONFIG_NYABULA_CORE_MOCK_CAPABILITIES` 保留确定性 provider：UI 输出
`nymock-ui[id]`，AI 原样返回 prompt。它用于回归测试，不代表真实 Eye 或模型
服务。生产配置不会静默使用mock。启用`NYABULA_CORE_EYE`后，未注册的UI
provider自动使用Eye服务；Display未启动返回`-ENODEV`。未接入的AI仍返回`-ENOSYS`。

本次UI已通过有界队列投递到Display所属的Eye渲染线程，`ui.eye`与`ui.notify`
共用权限，不直接跨线程操作LVGL。AI到ai_agent或Linux AMP服务的接入不在此PR内。
