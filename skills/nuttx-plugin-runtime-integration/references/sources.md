# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:85 feat(core): 集成 Eye Engine 与 Nyabula Display](https://github.com/open-vela/contest2026_062_PharosTech/pull/85)
- [team:91 Nyabula Core: add native product services](https://github.com/open-vela/contest2026_062_PharosTech/pull/91)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [app/nyabula_core](https://github.com/BeaconCat/contest2026_062_PharosTech/tree/e2aae2f858e85def2dc1fc4661534faa1685fc15/app/nyabula_core)
- [tools/nyabula_plugin/WASM_SDK.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/nyabula_plugin/WASM_SDK.md)
- [tools/nyabula_core/async-completion.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/nyabula_core/async-completion.md)
- [tools/nyabula_core/storage-contract.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/tools/nyabula_core/storage-contract.md)

## 项目内历史资料

以下名称供持有项目资料者定位；本Skill的操作方法与案例摘要已包含在runbook，不要求使用者拥有作者机器。长篇串口/录音/模型文件没有随包再分发。

- `工具/Core交接/Nyabula_Core_M1-M4_四轮交付报告_2026-09-06.md`；取材文件SHA256 `d3c31e1b49e0c72100018a37f26e53d1cbe2fdd375ef88b3845ebaf749a2c6e1`。

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
