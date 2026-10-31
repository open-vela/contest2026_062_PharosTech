---
name: rockchip-openvela-porting
license: Apache-2.0
description: 将新的 Rockchip ARM64 板移植到 openvela/NuttX，或定位构建、镜像交接、早期串口、GIC/MMU 与 NSH 启动故障。提供 BSP 代码入口、启动契约、故障判别和 K7 实证；已有系统的 N-Boot 刷写恢复使用 nboot-control。
---

# Rockchip → openvela 移植

把目标板推进到当前要求的启动或外设验证节点。经验来自 RK3576/K7；
地址、IRQ、GIC 版本、加载地址、DDR 保留区和引脚不能跨板照搬。

## 开始时只收集影响下一步的信息

从现有记录/源码取得目标型号、构建配置、启动链及最近失败输出。
只补问会改变下一步的缺项，不先要求用户填写完整表格。
没有板子时可继续完成源码、链接和镜像检查，结果不写成板测。

先读当前仓库的指令和最近实测记录。旧路线图只作为历史线索。
查找同 IP 的 NuttX 实现，逐项比对地址、IRQ、时钟、引脚和寄存器语义。
运行成功的联合配置不能自动证明其中每项条件都必要。

## 选择资料

| 当前任务 | 按需阅读 |
|---|---|
| 新移植：改哪些文件，如何接入构建 | [source-map.md](references/source-map.md) |
| 当前最早没通过的启动节点 | [workflow.md](references/workflow.md) |
| 无输出、异常、假构建成功、重启失效 | [triage.md](references/triage.md) |
| K7 从源码构建与打包 | [k7-runbook.md](references/k7-runbook.md) |
| FIT、入口或加载失败 | [boot-and-packaging.md](references/boot-and-packaging.md) |
| 时钟/引脚/SDMMC/SDIO/DMA | [driver-bringup.md](references/driver-bringup.md) |
| 核对 K7 数值、案例与过时路线 | [kickpi-k7-case.md](references/kickpi-k7-case.md) |
| 写实验结论 | [evidence.md](references/evidence.md) |
| 验收 Skill 本身 | [acceptance.md](references/acceptance.md) |

最近一次已执行的验证见 [skill-validation.md](references/skill-validation.md)。

## 工作路径

先区分已有 K7 复现与新板移植，再检查：
实际编入的代码及产物身份、loader 交接、早期可观察点、异常/内存/中断、NSH，
最后才是本次需要的外设。MMU/EL 初始化的具体先后由启动路径决定，
不是按里程碑标题机械重排汇编。

这是依赖顺序，不是重复执行清单。已有可信实测的环节无需重跑；
SMP、AMP、音视频和 AI 仅在任务需要时扩展。
从系统/驱动已有接口接入，不为一个板型重新写通用框架。

遇到故障，先用 triage 表选择能区分原因的观测。需要联合改动才能启动时
如实记录，后续再做消融；不强制把不可拆的启动契约拆成单变量试验。
静态寄存器一致不能排除上电窗口/关联器件条件，见案例 E3。

## 证据与报告

每条新记录包含场景、操作、结果、结论、置信。使用：
- 实测确认：在指定真机观察到结果；注明板卡、固件、测试范围。
- 编译通过：编译/链接；模拟器与宿主测试另外明确标识，不能冒充真机。
- 仅推断：由 TRM、DTS、源码或经验推导，尚未执行。

结果变化时保留历史原文，在当前说明中标注哪些假设已被推翻。
校验脚本只检查记录结构，**不能认证板测真实性、正确性或参赛合规性**：

```sh
python scripts/validate_evidence_log.py <实测日志.md> --json
python scripts/validate_evidence_log.py <本次整理记录.md> --strict
```

路径以本 Skill 目录为基准。不要为让 strict 通过而改写原始 AI 会话或
历史日志；单独整理带来源的实验记录。记录“失败”也是有效证据。

向用户报告：当前通过的里程碑、具体证据、剩余边界、下一步。
达到此次验收条件后交付，不自动扩展到其他硬件或发布操作。

## 停止条件与职责边界

到达用户要求的节点后给出版本、复现命令、输出及剩余缺口。启动到 NSH
不自动扩展成完整 WiFi、蓝牙、AI 或产品开发。

- N-Boot 日常维护交给独立 `nboot-control`；该 Skill 不可用时，查匹配版本
  的 N-Boot/板端 README，不猜命令。这里保留镜像交接与格式诊断。
- 芯片/板级首次移植采用 source-map；已有官方驱动开发 Skill 时复用其通用
  NuttX 分层规则，本包只补 Rockchip 的地址、时钟、DMA 与案例差异。

- 全量 openvela 工程使用 repo + manifest；不要手动拼接零散源码仓。
- 板级连接/产品策略放板层；芯片头仅放芯片事实；公共 API 集中声明。
- 写盘前确定介质、格式、槽位、大小及恢复方式；已有授权不重复询问。
- 当前 K7 产品使用 N-Boot A/B；k7flash/旧固定扇区 FIT 属于历史路线，
  不可用于替换 N-Boot 系统中的普通应用固件。
- 驱动初始化放正确生命周期；PIO/中断/DMA 分别验证，保留错误后恢复。
- 贡献代码遵循仓库许可证、命名和英文注释要求；每提交只做一件事。
- 上游实现与二进制须逐件核实许可证。“官方提供”不等于 Apache 授权。
- 本地构建/板测不自动授权 push、发布或合入 PR。
