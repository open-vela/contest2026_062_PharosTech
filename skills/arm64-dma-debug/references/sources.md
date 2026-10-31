# 来源与验证范围
取材于2026-09-20项目审计快照。这里的历史实验属于其原版本，整理Skill没有重新构建固件或板测。PR后续状态和默认分支可能变化，代码链接固定到下列已读取tree。
## 原始PR

- [team:36 chips/rk3576: ARM PL330 (DMA-330) DMA controller driver](https://github.com/open-vela/contest2026_062_PharosTech/pull/36)
- [team:45 实现DMA分配器](https://github.com/open-vela/contest2026_062_PharosTech/pull/45)
- [team:54 emmc: add ADMA2 and HS200/HS400 support](https://github.com/open-vela/contest2026_062_PharosTech/pull/54)
- [nuttx:10 usbhost: add USB Audio Class PCM streams](https://github.com/BeaconCat/nuttx/pull/10)

## 代码定位

团队源码定位版本：`e2aae2f858e85def2dc1fc4661534faa1685fc15`。这里只定位，不表示所有案例都在这个commit上执行。

- [chips/rk3576/rk3576_dma_alloc.c](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/chips/rk3576/rk3576_dma_alloc.c)
- [chips/rk3576/rk3576_dma.c](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/chips/rk3576/rk3576_dma.c)
- [chips/rk3576/rk3576_emmc.c](https://github.com/BeaconCat/contest2026_062_PharosTech/blob/e2aae2f858e85def2dc1fc4661534faa1685fc15/chips/rk3576/rk3576_emmc.c)

## 证据边界

runbook中的案例是历史结果摘要；“验收情境/回归集合”是后续使用时要检查的行为，不是声称本轮全部跑过。宿主测试、目标编译和真实硬件结果分别报告。包内没有模型权重、vendor库、私有固件或个人凭据；源码许可证不替代这些外部资产的许可。
