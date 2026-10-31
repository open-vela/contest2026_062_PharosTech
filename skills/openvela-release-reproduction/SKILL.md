---
name: openvela-release-reproduction
license: Apache-2.0
description: "把能在开发树运行的openvela项目整理成可复现的构建交付，固定多仓manifest、未提交补丁、生成资源、外部输入和产物身份。用于CI/干净工作区差异，不重复通用环境安装教程。"
---

# openvela 交付构建复现

先定义要复现的配置和产物，不把“最新代码”当版本。使用repo+manifest恢复工程，不拼接几个单仓clone。

1. 保存resolved manifest及每个dirty repo的补丁/应用顺序，工具链版本、最终.config、生成资源和外部输入hash；一份团队仓SHA不代表整个工程。
2. 检查linkfile实际目标和使用的头文件/源码树；多worktree场景中当前目录不是编译源身份。
3. 构建前置由正式入口幂等执行：依赖补丁、字体、配置与固件输入。Make/CMake各自验证，不靠构建机残留目录。
4. 明确当前distclean会删什么，保护工作修改和离线依赖；不要自动清空共享构建树来证明“干净”。
5. 输出记录ELF/裸镜像/包分别的hash、入口和来源，不让旧Make产物替代失败的CMake输出；公开占位构建不冒充可运行固件。
6. 从独立工作区复现到用户要求的节点，失败保留完整错误和缺失输入；没有板测只报告构建，不自动推送或发布。

[复现清单及失败案例](references/runbook.md)、[源码](references/sources.md)。优先复用工程既有setup/build/package脚本，不另造一套旁路。
