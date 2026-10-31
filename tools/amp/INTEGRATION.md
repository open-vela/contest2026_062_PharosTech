# 集成基线

- 团队PR #83：13aebd5a6919cbeab08342d79d814bdae4a40cca。
- N-Boot PR #7：27eddbe463a2dca688db5edfde965f3d0f396b5e。
- NuttX：e02f581e235fc7b527d57ff62b668ce625d139ab及附带GIC补丁。
- Linux：K7 Android14 SDK b553a938ddb56541f86507050f51af76c9929beb的kernel-6.1。

原分支不改写，AMP分别在新分支开发。团队保留最新LCD late initialize、
nbootctl与manifest链接；N-Boot保留#7的早期DM、串口恢复与持久一次性请求。

最小交付是显式RAM FIT启动与真实health/info，不改变默认bootcmd。
新N-Boot proper仍用团队工具包装成完整vendor FIT，再交给nbootctl更新。
不复制Linux GPL源码进Apache BSP；团队仅携带Linux配置/DTS与构建工具。

公共NuttX仓暂不另开PR。GIC补丁随团队仓提供，在CI与独立构建工作区显式应用；
后续提交公共仓并合入后再移除这项临时依赖。

## 模型交付（BLOB 服务）的方向约定

product 形态下 eMMC 与 `/data` 归 openvela，Linux 计算域无存储。`NYAMP_SERVICE_BLOB = 9`
因此是唯一「Linux 请求、openvela 应答」的服务。request_id 归发起方（计算域发起的置
bit 63），generation 仍只属于计算域并由应答回显，RESPONSE/EVENT 永不被应答，共享 arena
仍只有 Linux 一个分配器。完整规则与 opcode 表见 [protocol/README.md](protocol/README.md)，
设计与上板步骤见 [../../docs/amp-model-delivery.md](../../docs/amp-model-delivery.md)。

openvela 侧由 `app/nyabula_core/ny_compute.c`（`CONFIG_NYABULA_CORE_COMPUTE`）常驻持有
`rpmsg-raw` 端点的读端并应答 BLOB；`nyampctl` 在该服务运行时经其 port 收发，未运行或
最小 AMP profile 下仍按原方式独占端点。
