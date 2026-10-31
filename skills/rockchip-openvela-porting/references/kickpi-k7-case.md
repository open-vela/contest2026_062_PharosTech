# KICKPI-K7 可追溯案例

源码定位基线：团队仓 `0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b`。
各实验仍是各自日期的历史镜像，不是均在该commit上执行；E7另列其远端head。
本文件是内部实测日志的精选摘要，并非本次重新上板的报告。
原始串口日志及内部门槛记录未随本 Skill 全量分发；评审时可按下列日期、
完整标题定位项目根目录的 `实测日志.md`。缺少原件时只能引用历史报告，
不能声称自己复现。

## 架构合同及对应来源

| 合同 | 适用范围 | 来源 |
|---|---|---|
| UART0 0x2ad40000、1500000 baud | K7/RK3576 调试串口 | E1 及同日“板到手·串口链路通·A1 首轮摸底” |
| BL33 0x40200000、观测入口 EL2 | E1 的 2026-07-04 直接 BL33 镜像 | E1；N-Boot/AMP 须读各自当前交接合同 |
| GIC-400/GICv2，外设区从0x20000000开始 | RK3576 芯片配置，不能跨 SoC 套用 | [chip.h](https://github.com/open-vela/contest2026_062_PharosTech/blob/0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b/chips/rk3576/include/chip.h)；另查目标 TRM |
| 四 A53 openvela + 四 A72 Linux | 2026-09-10 特定 AMP 镜像 | E4；不代表所有 SMP/外设组合已验收 |

## E1：小镜像启动成功不证明大镜像安全

日期/标题：2026-07-04「★ 启动件全 rkbin 官方化: 自建 FIT 点亮 NSH,消除设备抠出 blob + atf-3 Bad hash」。

报告原始观察：`Entry 0x40200000`、`SPSR 0x3c9(EL2)`、
`NuttShell (NSH) nsh>`；对应串口日志 `console_20260704_144312.log`。
历史测试使用内嵌 FIT。仅证明当时镜像成功启动，不能把“内嵌更安全”
推广为产品路线；后来 E2 推翻了这一泛化。

## E2：载荷增长跨过 SPL 内存边界

日期/标题：2026-07-06「★ FIT 大nuttx启动崩根治:build_sd 改 mkimage -E(外部数据)」。

报告对照：462 KB 镜像能启动，503 KB 镜像出现 `No matching DT`；
修改为 `mkimage -E -p 0x1000` 后，503 KB 版本五段校验并进入 NSH。
这是当时 loader/布局的结果，**不是所有 Rockchip 的大小阈值，也不意味着
使用 -E 后镜像可无限大**。最终仍受目标分区、RAM、SPL 对齐规则限制。
现行产品打包已转 N-Boot，入口见 runbook。

## E3：SDIO host 寄存器一致仍可能缺少 combo 上电条件

日期/标题：2026-07-13「★★★★★★ CMD5 破案!! SV6621 首次应答: R4=0x90ffff00 (ready)」。

原文：
```text
SDIOPROBE: drive=0 sample=0 CMD5 RINTSTS=00000004 RESP=90ffff00  <<< REAL RESPONSE!
```

实验同时补齐 BT_RST/BT_WAKE、UART4 空闲电平及上电窗口自由运行时钟。
联合配置后取得真实响应，但日志明确说明**最小必要集未二分**。
因此“共享上电条件值得检查”可复用；“其中某一根脚是唯一根因”仍无证明。
芯片身份由实际 vendor/device/chip-id 验证为 SeekWave，旧 RTL/AP DTS
标签不能作为识别依据。

## E4：AMP 需要明确双端通知和地址发现契约

日期/标题：2026-09-10「AMP四小核openvela＋四大核Linux真实RPMsg闭环跑通」。

原文：
```text
nyamp health ok: generation=54195491 capabilities=0x00000001
online=4
```

修复接收使能、资源表 CPUNAME 后，Linux 端 READY 首帧完成动态地址学习。
该轮证明实际 health/info 往返，未证明 NPU推理、产品外设共存、
独立重启或完整隔离。后续能力须另找对应日期证据。

## E5：热更新成功的范围要具体

日期/标题：2026-09-10「nbootctl完成SD/NuttX域定向重启、clone、stage和N-Boot自更新闭环」。

原文：
```text
clone nuttx b -> a: 720544 bytes, verified
stage nuttx a: 921600 bytes, version 10, activated
verify nuttx a: OK
update-nboot: 4194304 bytes written and verified on sd
FINAL_BOOTCTRL_PASS copies-identical CRC-valid AMP-unchanged request-cleared active-A 50
```

适用范围仅为该轮 SD/NuttX 配置，不含 eMMC、AMP 写入或掉电故障注入。
先前关于 CRU 复位能保留 PMU 请求的推断被推翻，最终使用持久 bootctrl。
[当前接口与限制](https://github.com/open-vela/contest2026_062_PharosTech/blob/0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b/app/nbootctl/README.md)。

## E6：构建和协作者板测不能混为一谈

日期/标题：2026-09-11「GPIO TE及GC9B72配置修复通过完整CI」。

[CI run 34599585688](https://github.com/open-vela/contest2026_062_PharosTech/actions/runs/34599585688)
证明对应版本构建成功。屏幕点亮来自协作者反馈；该轮未新增帧率和
长稳实测。之后 TE 路线又改为 boardctl，旧 /dev/gpio1/2 操作不能
作为当前默认。查当前源码/配置后再复用。

## 已知旧资料偏差

早期施工图中的 GICv3、手工 clone 多仓、RTL8822CS、UART4 蓝牙数据通路、
“eMMC 从未实例化”、k7_sdpack 路径均不能直接写入新流程。
发现冲突时记录来源和日期，以当前源码及对应实验为准。

## E7：已有板测不代表全新构建能复现

来源：2026-09-20 [团队PR93](https://github.com/open-vela/contest2026_062_PharosTech/pull/93)，
head `e2aae2f858e85def2dc1fc4661534faa1685fc15`，
[构建run35494348153](https://github.com/open-vela/contest2026_062_PharosTech/actions/runs/35494348153)。
本次9月20日审阅实际读取了该head的失败日志；没有重新构建或板测。

原始错误摘录（不是完整日志）：

```text
Prepare Eye fonts: python3 app/nyabula/tools/generate_fonts.py
ny_agent.c:729:53: error: 'agent_msg_t' has no member named 'request_id'
fatal error: nuttx/config.h: No such file or directory
```

同日已有产品板测记录，但这个CI中字体准备、Agent依赖和独立协议测试的
构建边界尚未闭环。可迁移的结论是检查生成资源、补丁/依赖版本与实际树；
不是断言所有新故障都由缺补丁引起，也不是抹掉先前板测。
该head之后是否修好需查对应新版本结果，不能沿用这条记录判定当前CI。
