# Profile、隔离和反例

## Nyabula入站profile是有限实现

既有实现面向MCP 2025-11-25的stateless JSON-only POST，initialize/initialized/ping/tools/list/tools/call；GET/DELETE拒绝，不提供SSE/session/sampling/elicitation/tasks。这是项目固定profile，不声称代表最新MCP全部能力。

本地接入只接受带端口localhost/127.0.0.1 Host，独立Bearer凭据，拒绝Origin。把它改成LAN/公网服务是另一个威胁模型和部署任务，不能仅把bind地址换成0.0.0.0。

原实现上限包含8客户端、8scope、4KiB请求头、8KiB请求体、32KiB响应、每连接5s，且无大结果分页。容量可随产品设计改变，但必须对应有界buffer与行为测试，不用无限读解决截断。

## 成功证据

9月12日真实NuttX sim及浏览器联测覆盖入站只读工具、独立凭据、主人/外部history隔离、跨客户端cancel拒绝、scope授予/撤销和真实ai_agent回合；原主人聊天/审批/Skills链回归通过。部分HTTP peer为受控测试服务，不推成全部真实微信/飞书/公网TLS已联调。

## 易错边界

- JSON字符串ID和精确整数ID保真，拒绝cJSON会截断的转义NUL；不能宽松转成double丢身份。
- 流式socket的EAGAIN表示暂不可读，不把它变成每次一字节的低效读取或EOF。
- 工具调用已提交但审计/网络响应失败时可能丢响应，重试先查状态，不能默认重做副作用。
- 只列scope允许的工具；测试模型提出恶意工具名，也不能绕过执行层检查。

## 验收输入

用两个不同凭据发相同requestId、尝试读取彼此history与cancel；撤权后重发旧请求；错误Origin/Host、重复Authorization和畸形UTF-8；工具执行后断开再查询。每个应有确定结果，不通过删鉴权或放宽scope“让端到端通过”。
