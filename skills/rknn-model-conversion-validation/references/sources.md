# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:87 AMP: 板载 AI 与摄像头计算服务对接草案](https://github.com/open-vela/contest2026_062_PharosTech/pull/87)
- [team:93 Nyabula 产品固件：Core 产品服务、AMP 端侧 LLM 与工具调用、升级与模型交付](https://github.com/open-vela/contest2026_062_PharosTech/pull/93)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [tools/amp/models/nyamp_melo.cpp](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/amp/models/nyamp_melo.cpp)
- [tools/amp/models/README.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/amp/models/README.md)

## 项目内历史资料

以下名称供持有项目资料者定位；本Skill的操作方法与案例摘要已包含在runbook，不要求使用者拥有作者机器。长篇串口/录音/模型文件没有随包再分发。

- `工具/MeloTTS_NPU试验_20260913.md`；取材文件SHA256 `405a7e187d6f59499b41cb240b71e2bd6f23cce8a88153d6b1058485341d796a`。
- `工具/认主人脸识别模型选型_20260913.md`；取材文件SHA256 `df12f368383aca9ab16fc95d9e709bfe4a9c590238a1540f00aa5e525f5290c2`。

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
