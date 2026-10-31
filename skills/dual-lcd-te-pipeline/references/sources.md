# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:59 添加st77916屏幕驱动以及gc9b72屏幕驱动](https://github.com/open-vela/contest2026_062_PharosTech/pull/59)
- [team:75 Nyabula display：lvgl兼容层](https://github.com/open-vela/contest2026_062_PharosTech/pull/75)
- [team:85 feat(core): 集成 Eye Engine 与 Nyabula Display](https://github.com/open-vela/contest2026_062_PharosTech/pull/85)
- [team:86 极大幅度地提升了nyabula_eye渲染引擎的效率与视觉效果](https://github.com/open-vela/contest2026_062_PharosTech/pull/86)
- [team:94 nyabula_eye：修复渲染错误，以及一些细节的修正](https://github.com/open-vela/contest2026_062_PharosTech/pull/94)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [app/nyabula_display](https://github.com/BeaconCat/contest2026_062_PharosTech/tree/e2aae2f858e85def2dc1fc4661534faa1685fc15/app/nyabula_display)
- [app/nyabula/SCENE_SCHEMA.md](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/app/nyabula/SCENE_SCHEMA.md)

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
