# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:58 audio: 完善 K7 ES8388 双麦、全双工与宽位宽音频](https://github.com/open-vela/contest2026_062_PharosTech/pull/58)
- [nuttx:3 audio: 完善 ES8388 双向生命周期与 PCM 边界修复](https://github.com/BeaconCat/nuttx/pull/3)
- [nuttx:4 音频/es8388：修复错误的左右声道音量设置](https://github.com/BeaconCat/nuttx/pull/4)
- [nuttx:5 音频/es8388：修复错误的i2c读取字节数引发的越界](https://github.com/BeaconCat/nuttx/pull/5)
- [nuttx:6 音频/es8388：不在`es8388_reset`里重置priv结构体的值](https://github.com/BeaconCat/nuttx/pull/6)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [chips/rk3576/rk3576_sai.c](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/chips/rk3576/rk3576_sai.c)
- [app/audioctl](https://github.com/BeaconCat/contest2026_062_PharosTech/tree/e2aae2f858e85def2dc1fc4661534faa1685fc15/app/audioctl)

## 项目内历史资料

以下名称供持有项目资料者定位；本Skill的操作方法与案例摘要已包含在runbook，不要求使用者拥有作者机器。长篇串口/录音/模型文件没有随包再分发。

- `工具/音频完整矩阵与短稳态验证_R16.md`；取材文件SHA256 `c6ca6ebd0c16182c0c7e2e564cd2790fbe8495b5e84063ad675cfb2f63d50a89`。
- `工具/24位播放修复与验证_R20.md`；取材文件SHA256 `115e6fa6d492b9c728059c41538541773d345702152b7baf50f7e5fd8b191a44`。

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
