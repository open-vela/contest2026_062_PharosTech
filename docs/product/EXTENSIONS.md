# 扩展 Nyabula

[产品总览](README.md) · [系统架构](ARCHITECTURE.md) · [插件SDK](../../tools/nyabula_plugin/README.md)

想增加一个自己的工具，可以让设备Agent生成，也可以用SDK开发后安装。想让外部Agent访问设备，则使用受限MCP。三条入口共用设备权限与状态，不互相绕过授权。

## 让 Agent 创建工具

1. 在Nyabot配置可用的在线模型账户。
2. 主人在“扩展”页开启“自动生成与安装”。
3. 描述具体需求，例如“做一个记账本，记录金额、分类和备注”。
4. 查看执行步骤和实际页面；继续反馈“负数应拒绝”“增加分类筛选”等。

生成过程读取[生成契约](../../tools/nyabula_plugin/authoring-contract.txt)，检查已有版本，生成自己签名的 `agent-*` 插件并通过实际UI事件测试。已有[记账本](../../tools/nyabula_plugin/examples/local-ledger/README.md)、[传输计算器](../../tools/nyabula_plugin/examples/transfer-planner/README.md)与[硬件页面示例](../../tools/nyabula_plugin/examples/hardware-panel/README.md)。

候选首次启动要发布有效页面；版本冲突拒绝覆盖，失败恢复旧代码、私有数据与授权。模型回复不是执行凭据，实际副作用由Core步骤与结果确认。

## 安装和使用运行时 Skill

在技能页导入[transfer-planning/SKILL.md](../../tools/nyabula_plugin/skills/transfer-planning/SKILL.md)，预览后保存，再由主人启用。它指导模型区分GB/GiB、Mbps/MB/s和效率；B64已有实际读取并生成、修复传输计算器的记录。

规则：UTF-8正文最多8192字节；保存或修改后停用；启用后进入可读索引，Agent可分页读取完整正文。模型请求保存或启用技能需要主人审批。技能内容只提供知识，不增加工具、权限或审批豁免。

开发代理使用的[24份工程Skill](../../skills/README.md)是另一类资产：帮助完成平台移植、模型适配和验证，不等于设备已安装24个运行时技能。

## 用 SDK 编写插件

TypeScript/JavaScript走QuickJS，C/Rust/TinyGo使用WAMR与各自构建约束。SDK提供manifest、构建、签名打包、能力接口与示例。详细命令以[SDK README](../../tools/nyabula_plugin/README.md)和[Wasm说明](../../tools/nyabula_plugin/WASM_SDK.md)为准。

真实设备可从面板上传 `.nya` 或 `.zip`，也保留目录安装入口。包根包含manifest，设备验证签名、信任与完整性；分块上传和解包限制由SDK说明定义。新装插件保持停止，声明能力后仍需主人授权。

NyaUI支持组件树与隔离HTML，宿主切换视图。页面输入/事件只使用发布过的绑定；停止或撤权后页面失效。图片与iconfont可以随受签名资源发布，字体/图片各自保留许可来源。

### 硬件访问

JS插件通过 `@nyabula/hardware` 的受控topic接口读取和操作设备：

```ts
import { request } from "@nyabula/hardware";

// Declare audio.read, then let the owner grant it.
const status = await request("audio.status");
```

声明权限不等于授权，每次调用检查当前状态。摄像头采集权限名已经存在，但当前产品采集服务未提供时返回 `ENOTSUP`；不能从权限表推导该功能已经集成。

## 双向 MCP

| 方向 | 用途 | 访问条件 |
|---|---|---|
| 出站 | Nyabula发现并调用外部HTTPS工具 | 主人选择工具，逐次批准具体参数；schema变化重新确认 |
| 入站 | 外部Agent读取或操作被授权的设备能力 | loopback监听，通过ADB/SSH隧道；限期、限scope凭据 |
| 开发工作台MCP | 加载本机源码、设置夹具、调试UI/日志 | 仅可信开发客户端；不等同生产入站MCP |

入站客户端不能读取主人聊天、跨客户端取消任务或替主人审批。撤销、到期和关闭开关会阻止后续访问。出站写入结果不确定时不自动重试。

微信iLink和飞书等渠道已有适配与协议测试，真实账号扫码/收发仍需部署时验收。完整接口与测试入口见[Agent工作台说明](../agent-workbench.md)。

## Windows 工作台与 Cloud

Windows独立工作台以Goja执行可信开发插件，可加载/重载、授予权限、切换组件/HTML/JSON、操作测试私有存储、注入故障、模拟设备并用19项开发MCP控制。板上QuickJS的签名、硬件和内存约束仍需真机验证。

Go Cloud提供账号、设备认领、统计、中继与Console静态服务。K7主动连接Cloud，认领账号与设备访问密码分别检查；WSS验证证书链和主机名。已做原生K7本机服务联调，公网生产部署另行验收。

**源码交付位置：** 当前团队仓包含设备Cloud客户端、Console界面、插件SDK与示例；Go Cloud服务和Windows工作台为独立工作区/开发包，未纳入本仓manifest。取得配套包后按其README启动，不能把 `repo sync` 视为已包含这两个Go工程。
