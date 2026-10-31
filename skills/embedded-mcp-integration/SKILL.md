---
name: embedded-mcp-integration
license: Apache-2.0
description: "在设备产品中接入MCP客户端或服务器，处理独立凭据、principal/scope、工具发现、逐次审批、异步取消和会话隔离。用于真实设备能力适配，不默认公开监听或连接外部账号。"
---

# 嵌入式 MCP 接入与权限验证

先区分设备调用外部MCP与外部Agent调用设备MCP，明确谁是principal、谁持权限、传输profile及已有授权范围。

- 将协议解析、认证、scope、工具适配和业务执行分层；工具名字/模型返回不能授予权限。
- 固定实现支持的协议版本与HTTP能力，严格处理ID、UTF-8、NUL、重复关键头和长度；不支持的SSE/session/tasks明确拒绝，不能半实现。
- 外部会话与主人会话分别保存history、memory/context和request状态；取消只作用于该主体拥有的请求。
- 对副作用工具沿用Broker的审批/幂等路径；连接超时后先查requestId，不能无条件重发造成重复操作。
- 撤销凭据/权限要使新调用失败，并按合同终止在途请求；审计记录来源/工具/结果，不记录密钥。

[profile、隔离和反例](references/runbook.md)、[来源](references/sources.md)。交付allow/deny/revoke/cancel及跨主体回归，不把受控mock对端说成真实平台账号联调。
