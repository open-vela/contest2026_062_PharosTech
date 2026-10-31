# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:60 openvela/NuttX: add portable SV6621 WiFi driver with RK3576 backend](https://github.com/open-vela/contest2026_062_PharosTech/pull/60)
- [team:70 ci: 使用全零占位固件验证完整构建](https://github.com/open-vela/contest2026_062_PharosTech/pull/70)
- [team:72 修复 WiFi 与 RTC 的 GPIO/I2C 冲突](https://github.com/open-vela/contest2026_062_PharosTech/pull/72)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [drivers/drivers/sv6621](https://github.com/BeaconCat/contest2026_062_PharosTech/tree/e2aae2f858e85def2dc1fc4661534faa1685fc15/drivers/drivers/sv6621)
- [drivers/include/sv6621.h](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/drivers/include/sv6621.h)

## 项目内历史资料

以下名称供持有项目资料者定位；本Skill的操作方法与案例摘要已包含在runbook，不要求使用者拥有作者机器。长篇串口/录音/模型文件没有随包再分发。

- `工具/SV6621_WIFI剩余工作清单.md`；取材文件SHA256 `a2dca4980c012a6c3c02b1bf6fc5a8f3677388db26f9e2f54b25e72d8af41237`。

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
