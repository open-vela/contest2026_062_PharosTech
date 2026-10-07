# RK3576 VEPU510 H.264 编码器驱动

本目录是 openvela / NuttX 上 RK3576 硬件视频编码器 **VEPU510** 的内核侧驱动。
VEPU510 位于 `0x27a00000`，在 `PD_VEPU0` 电源域内，IP 本身支持 H.264、H.265 和
JPEG 编码；本驱动目前只驱动 **H.264、固定 QP** 这一条路径，并把它呈现为一个
V4L2 mem-to-mem 设备节点：output 队列收 NV12 原始帧，capture 队列出 annex-B
H.264 码流。

板级入口在 `boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c`：先调用
`rk3576_vepu_initialize()` 完成电源、复位、时钟与版本探测，再调用
`rk3576_vepu_codec_register("/dev/video1")` 注册设备节点（`/dev/video0` 归 VICAP
摄像头）。参考应用是 `app/camenc`。

## 代码结构

| 文件 | 职责 | 编译开关 |
| --- | --- | --- |
| `rk3576_vepu.c` | 电源域 / 复位 / 时钟 bring-up、版本探测、一次编码的 start 与 finish、状态块解码、开机自检 | `CONFIG_RK3576_VEPU` |
| `rk3576_vepu.h` | 上述硬件层的公开接口 | 同上 |
| `vepu510_regs.c` / `.h` | 一帧该往寄存器里写什么值（控制块、源格式、地址、码流语法、固定 QP 率控块） | 同上 |
| `vepu510_tables.c` / `.h` | 寄存器层使用的 tuning 表：RDO lambda、anti-flicker 阈值与权重 | 同上 |
| `h264_syntax.c` / `.h` | SPS / PPS 生成与 RBSP bit writer；切片头由硬件从 `synt_sli0/1/2` 寄存器综合，软件不生成 | 同上 |
| `rk3576_vepu_codec.c` / `.h` | V4L2 M2M 设备节点、缓冲区与 streaming 状态机、作业调度 | `CONFIG_RK3576_VEPU_CODEC` |
| `hardware/rk3576_vepu510_reg.h` | 寄存器布局模型（**生成文件**，勿手改；由上游 MPP 的寄存器头机械转换而来，并内嵌布局断言） | 随 `rk3576_vepu.c` |

## 上电与探测次序

1. 释放 `PD_VEPU0` 的 memory-repair 初复位，并保持该电源域供电。**该复位是保持
   状态**，未释放前域内每个寄存器都读回 0——看起来就像驱动什么都没写。
2. 释放 CRU 软件复位。
3. 打开时钟，包括总线接口门控。`PD_VEPU0` 的 BIU 可以独立于时钟被挂起：这种
   状态下寄存器读写完全正常，却发不出一条 AXI 事务（`rk3576_vicap.c` 记录了
   `PD_VI` 上同样的陷阱）。
4. 读 VEPU510 版本寄存器并检查 `h264_cap`。这一步同时验证了电源、复位、时钟
   整条路径，是真正的端到端检查而不是冒烟测试。

时钟频率**不在这里设定**：寄存器访问只需要 `aclk`/`hclk`，core 时钟由驱动在能够
测量候选父时钟之后按设备树目标设置（ACLK 400 MHz、core 702 MHz，见
`hardware/rk3576_vepu.h`）。

## 应用接口

* **设备节点**：`/dev/video1`。
* **格式**：output 侧只接受 `V4L2_PIX_FMT_NV12`；capture 侧给出
  `V4L2_PIX_FMT_H264`，annex-B 封装，流的第一个访问单元前带 SPS 与 PPS，因此
  写进文件即可直接播放。
* **内存**：只接受 MMAP 缓冲区。
* **私有 control**：编号从 `0x1000` 起。它们必须是私有的，因为
  `v4l2_m2m.c` 的 `struct v4l2_ext_control` 把 `id` 声明为 `uint16_t`（标准是
  `uint32_t`），所有 `V4L2_CID_MPEG_*`（`0x009809xx` 以上）都不可达，
  `V4L2_CID_PRIVATE_BASE`（`0x08000000`）同样放不进这个字段；框架的
  `query_ext_ctrl` 未接通，所以应用无法枚举，必须使用下表编号。

  | CID | 名称 | 取值 |
  | --- | --- | --- |
  | `0x1000` | QP | 0..51，每帧编码使用的量化参数 |
  | `0x1001` | profile | 66 / 77 / 100；低于 100 会同时去掉 8x8 变换和第二个色度 QP 偏移 |
  | `0x1002` | level | 0 表示由写入方选择几何尺寸所需的最小 level |
  | `0x1003` | deblock | 1 表示在 PPS 中携带去块滤波控制 |
  | `0x1004` | GOP | 每组图像数 1..1000；1 表示每帧都是 IDR，大于 1 时每组首帧为 IDR、其余为 P 帧 |

* **帧率**：不是 control，走 V4L2 已有的 `VIDIOC_S_PARM`。它只作为 VUI timing
  进入码流，告诉播放器如何呈现；本驱动没有码率控制，帧率不决定编码器跑多快。
* **等待模型**：`VIDIOC_QBUF` 提交作业后立即返回，作业在硬件中断（高优先级工作
  队列）上完成；应用用 `poll()` 等待码流，而不是让队列操作阻塞。
* **预测约束**：一组内非首帧不能单独丢弃——其后的图像从它预测，缺失的参考会让
  解码器把后续画面解错。需要丢帧的调用方应当改为让客户端从下一组重新开始
  （`app/camenc/camenc_ws.h` 中 `waiting` 的注释说明了这一约定）。

## Kconfig 选项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `CONFIG_RK3576_VEPU` | n | 驱动本体：电源/复位/时钟、寄存器层、作业层 |
| `CONFIG_RK3576_VEPU_SELFTEST` | y | 开机用内存中合成的图像跑编码路径验收测试 |
| `CONFIG_RK3576_VEPU_SEQ_DUMP` | n | 把序列测试的码流以十六进制打进日志，供主机侧解码器读取 |
| `CONFIG_RK3576_VEPU_CODEC` | n | 注册 V4L2 M2M 设备节点 |
| `CONFIG_RK3576_VEPU_CODEC_SELFTEST` | y | 开机通过设备节点本身跑一遍完整的应用侧流程 |

## 开机输出与自检

把 `CONFIG_RK3576_VEPU_CODEC` 和两级自检都打开时（`configs/camera` 如此），
开机固定打印五行：

```
VEPU0: aclk=... hclk=... core=... (targets ... and ...)
VEPU0: VEPU510 rev ..., ip_id 0x..., h264=... hevc=... bframe=... fbc=..., res=..., osd=..., filter=...
VEPU0 sequence test: N frames, N bytes: IDR N B mean, P N B mean (N% of an IDR)
VEPU0 self-test: N frames, N bytes of bitstream, solid L/S, gradient L/S, noise L/S (length/sse)
VEPU0 codec self-test: N frames through the device node, N bytes of bitstream
```

这五行报告每次开机都值得知道的两件事——**哪一颗 IP 应答了**、**编码路径是否
可用**——并且按可以独立损坏的层次各报一次：硬件、编码路径、应用实际面对的
设备节点。时钟频率之所以要打印，是因为 core 时钟若停在复位源上，编码器会以
几分之一的速度工作却什么都不说。

自检由此分两段：

* **序列测试**（`rk3576_vepu_selftest_sequence()`）把运动图像编成一个 IDR 加若干
  P 帧，检查 P 帧比它自己的参考帧更省。这一项失败意味着预测已经失效——这是
  一种无声的故障：码流照样能解码，只是码率不对。
* **图像测试**编码静止、渐变和噪声三种源，检查码流长度按源的复杂度排序，并检查
  NAL 在编码器声称的位置结束。

`CONFIG_RK3576_VEPU_CODEC_SELFTEST` 是同一件事在设备节点层再做一遍：协商格式、
申请并映射缓冲区、streaming、检查回来的码流，证明应用确实能到达编码器。

出错信息不受调试开关影响：`_err` 始终输出，编码失败时 `rk3576_vepu_dump()` 会
打印寄存器。其余细节（每个时钟分支落在哪个源、分频值、重建集大小、两段自检
的逐帧记录）走 `vinfo()`，由 NuttX 的 `CONFIG_DEBUG_VIDEO_INFO` 打开，默认关闭。

## 验证

移植的正确性不靠阅读保证。寄存器布局、寄存器取值和 SPS/PPS 都曾在主机上与上游
MPP 的对应实现并排编译对拍：

* 生成头文件的每一项 `sizeof`/`offsetof` 与上游一致（463 项），每个寄存器组的位宽
  和恰为 32 位；这些尺寸作为编译期断言内嵌在生成的头文件里，此后任何布局改动都
  会直接编译失败，而不是悄悄改变写进编码器的内容。
* `vepu510_regs.c` 写出的寄存器字与 MPP 的 `setup_vepu510_normal()` /
  `setup_vepu510_prep()` 逐字节一致。
* `h264_syntax.c` 生成的 SPS/PPS 与 MPP 的 `h264e_sps_to_packet()` /
  `h264e_pps_to_packet()` 逐字节一致。
* 打开 `CONFIG_RK3576_VEPU_SEQ_DUMP` 得到的十六进制码流可交给主机解码器复核。

## 范围与限制

* **固定 QP**：没有码率控制、没有 AQ、没有 ROI；帧率只进 VUI。
* **无 B 帧**：硬件报告 `bframe=0`，一组就是「一个 IDR + 若干 P」。
* **只驱动 H.264**：HEVC、JPEG 编码和硬件解码能力都没有接入。
* **只支持 NV12 输入、H.264 输出**，4:2:0、8 bit、progressive（`frame_mbs_only`）。
* **硬件一次只跑一个作业**，由互斥量保证；`rk3576_vepu_start()` 成功返回时持有
  该互斥量，必须由 `rk3576_vepu_finish()` 释放，两者之间不得阻塞在同样等待
  编码器的东西上。

## 许可证与来源声明

本目录代码以 **Apache-2.0** 分发。

其中若干文件是 **Rockchip MPP（Media Process Platform）** 的移植或摘录。
MPP 是 Rockchip 在 Linux 上驱动同一颗 IP 的用户态库，许可证与 Apache-2.0 兼容：

| 项目 | 内容 |
| --- | --- |
| 上游仓库 | `rockchip-linux/mpp`（https://github.com/rockchip-linux/mpp） |
| 分支 | `develop` |
| commit | `14729dd578e570e5f00fd1dd2113f5429012d64b` |
| commit 日期 / 标题 | 2026-09-17，`feat[vepu511]: Setup quant registers for H.264` |
| 版权 | Copyright (c) 2015-2026 Rockchip Electronics Co., Ltd. |
| 许可证 | Apache-2.0（仓库另有 `LICENSES/MIT` 双许可文件；未取用） |
| 上游 NOTICE | 无（因此 Apache-2.0 §4(d) 不适用） |

文件对应关系：

| 本目录 | 上游 MPP |
| --- | --- |
| `vepu510_regs.c` / `.h` | `mpp/hal/rkenc/h264e/hal_h264e_vepu510.c`、`mpp/hal/rkenc/common/vepu510_common.h` |
| `vepu510_tables.c` / `.h` | `mpp/hal/rkenc/common/vepu51x_common.c` |
| `h264_syntax.c` / `.h` | `mpp/base/mpp_bitwrite.c`、`mpp/codec/enc/h264/h264e_sps.c`、`h264e_pps.c`、`h264e_slice.c` |
| `hardware/rk3576_vepu510_reg.h` | `mpp/hal/rkenc/common/vepu5xx_common.h`、`vepu51x_common.h`、`vepu510_common.h`、`mpp/hal/rkenc/h264e/hal_h264e_vepu510_reg.h` |

**这些文件是经过修改的衍生作品**，不是原样引用：上游代码被裁剪、改写或转录，
并重新组织以适配 NuttX 内核构建，暴露的接口是本驱动的接口而非 MPP 的。上表中的
每个文件头部都带有一段来源与修改声明（上游文件、commit、Rockchip 版权行，以及
「这是修改过的衍生作品」的说明）。寄存器取值与码流语法已由上一节所述的对拍与
上游逐字节、逐字段比对。剩余文件（`rk3576_vepu.c`、`rk3576_vepu_codec.c` 等）是为
本驱动编写的原始代码，其中若干处注释引用了 MPP 的行为作为依据。

本仓不携带上游 MPP 源码：上述文件是重写或转录后的实现，完整的上游版权与许可证
信息见仓库 `rockchip-linux/mpp`（Apache-2.0）。

**许可证边界**：Rockchip 的内核态驱动
（`rockchip-linux/kernel` 的 `drivers/media/platform/rockchip/mpp/`）是
**GPL-2.0 only**，本项目**不使用**其中任何代码。VEPU510 寄存器手册属 Rockchip
机密文档，只作内部参考，其内容不随代码分发。
