# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:87 AMP: 板载 AI 与摄像头计算服务对接草案](https://github.com/open-vela/contest2026_062_PharosTech/pull/87)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [tools/amp/COMPUTE_VALIDATION.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/amp/COMPUTE_VALIDATION.md)

## 项目内历史资料

以下名称供持有项目资料者定位；本Skill的操作方法与案例摘要已包含在runbook，不要求使用者拥有作者机器。长篇串口/录音/模型文件没有随包再分发。

- `工具/媒体板测_20260912.md`；取材文件SHA256 `6b86f966a4476d90e3845e835767c339051a34d2b3a33b4f449b431e91d7642d`。
- `工具/OV5647-CSI0接线核验_20260912.md`；取材文件SHA256 `bac070a2c2de2666e89afcb3d9f8ffd6a26f5100408fb39bf4be7e3ab217a6e2`。

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
