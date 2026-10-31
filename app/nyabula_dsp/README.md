# Nyabula 软件 DSP

独立 C99 DSP 核心 + 调用同一动态库的 Python 离线 WAV 工具。
不修改 NuttX 的 `audio/pcm_decode.c`、ES8388、SAI 或 USB 驱动。
库仍可独立宿主构建；产品 Core 已让蓝牙、TTS 与文件播放接入公共 DSP 出口。使用入口、设备路由与验收边界见 [音频服务](../../docs/audio-plugin-services-20261018.md)。

## 已实现的处理合同

- 固定 48 kHz；C API 输入为交错立体声 float32，范围 [-1, 1]。
- 输出顺序：左高通、右高通、单声道低通，交错三通道。
- 全局主增益之后，每个输入声道在扩展接口中最多16段 EQ（旧接口为 peaking；`nyadsp_create_eq` 为四段类型扩展，`nyadsp_create_input_eq` 接受1–16段；均可指定低/高架或关闭）。
- 低频输入为 `(L + R) / 2`，不是直接相加；反相信号会抵消。
- 左右各两级二阶 Butterworth 高通、低频两级低通，构成 LR4。
- 每个输出有最多四段 peaking EQ、独立增益、极性和整数帧延迟。
- 三路共用瞬时攻击、约 80 ms 释放的采样峰值限幅增益，避免各路
  独立限幅改变瞬时电平比例。最后还有限制浮点舍入的钳位。
- 每个实例有独立状态；配置复制后不可变。reset 清除滤波器、延迟、
  限幅和统计状态；不改配置。
- 处理函数不分配内存、不读文件、不加锁。调用者必须单线程持有实例；
  create/reset/destroy 不得与 process 并发。
- 非法输入块（NaN/Inf、越界样本、重叠缓冲、超过 4096 帧等）
  在处理前整体拒绝，不推进内部状态，不修改输出。

这只是数字采样峰值保护，不是 true-peak 限幅、扬声器位移/温度保护、
功放功率限制或经过听感验证的成品动态处理。它不代表可以安全满功率
驱动喇叭。低音单元的保护高通、功放映射和实际音量上限需在硬件阶段定。

## 构建和验证

从队伍仓根目录运行（Linux，CMake + C99 编译器 + Python 3.10+）：

```sh
cmake -S app/nyabula_dsp -B out/audio-dsp \
  -DCMAKE_BUILD_TYPE=Debug -DNYADSP_SANITIZE_TEST=ON
cmake --build out/audio-dsp -j2
ctest --test-dir out/audio-dsp --output-on-failure
```

库为 `out/audio-dsp/libnyadsp.so`。Windows 可以使用 CMake/MSVC
生成 `nyadsp.dll` 后传给 Python；此阶段只验证了 Linux 宿主构建，
Windows 原生动态库尚未验收；产品 ARM64 B53 板测见音频服务文档。

ASan/UBSan 仅加在原生边界测试程序，不加在 Python 加载的动态库；
动态库的频响和文件回归由另一个 CTest 运行。测试不依赖 NumPy、
SciPy、REW 或音频设备。

2026-09-14：指定构建机 x86_64/GCC 14.2 上 Debug 与 Release 均通过
两组 CTest；最终 Python 回归共 20 项，通过。原生边界测试启用
ASan/UBSan。源码 clang-format 检查与 Python 语法检查通过。
这些结果不替代 NuttX 完整链接、ARM64 性能、板测或声学校准。
不要使用破坏 NaN/Inf 检查语义的 fast-math 编译选项。

## 离线处理

```sh
python3 tools/nyabula_dsp/render.py input.wav out/rendered \
  --library out/audio-dsp/libnyadsp.so \
  --config app/nyabula_dsp/presets/balanced.json
```

本阶段只接受 48 kHz、16-bit、双声道、未压缩 PCM WAV；
不自动重采样、不悄悄转换单声道或 24/32-bit。标准 RIFF 的其他
数据块可跳过。短文件、截断样本数据和不支持的格式会拒绝。

输出：

- `satellites.wav`：左右高通，立体声 PCM16。
- `bass.wav`：单声道低通，PCM16。
- `report.json`：输入/输出帧数、配置、动态库 SHA256、峰值及限幅帧数。

两文件采样率与帧数相同，但分开启动两个播放器不会自动同步。
它们用于离线分析，不代表 ES8388 + USB 同步播放已经实现。
试听使用低音量；不能据合成测试音认定箱体调音完成。

默认在输入末尾补“最大配置延迟 + 250 ms”静音，以排出延迟和滤波尾音。
可用 `--tail-ms 0` 取消额外滤波尾音，但仍保留最大配置延迟；IIR
理论尾音无限，有限补零不代表数学上完全排空。
`--block` 可选 1..4096，默认 1024，不影响输出样本。

目标目录必须不存在。先写同级临时目录，全部关闭后才发布结果；
失败清理本次临时输出，不覆盖已有结果或输入文件。

## 配置与安全旁路

`presets/balanced.json` 是平直 EQ 的开发起点，不是实测声学校准。
500 Hz 分频点只是可测试默认值，必须按实际单元、箱体与听感调整。

| 字段 | 范围 / 含义 |
|---|---|
| version | 1 |
| sample_rate | 48000 |
| crossover_hz | 40..5000 Hz |
| master_gain_db | -30..0 dB；默认 -6 dB 留余量 |
| ceiling | 0.1..0.99 满幅比例；默认 0.9 |
| eq | 最多四段；每段需要 frequency_hz、q、gain_db |
| EQ 参数 | 20..18000 Hz，Q 0.2..10，增益 -12..12 dB |
| outputs | left、right、bass，每项支持 gain_db、polarity、delay_frames、eq |
| 输出 gain_db | -24..12 dB |
| polarity | 1 或 -1 |
| delay_frames | 0..4800；48 帧 = 1 ms，最高 100 ms |
| bypass | true 时干声左右输出、低频静音；仍保留主增益和限幅 |

旁路跳过所有 EQ、分频、输出增益/极性/延迟，不是保留原路由的效果开关。
未知字段拒绝，避免拼写错误静默失效。未提供的合法字段用默认值；
零增益 EQ 段是精确直通。

单个实例配置仍不可变。产品播放器在处理块边界建立新实例，预热延迟与滤波历史，
再交叉淡化切换；不在旧实例中并发修改系数。新增 `nyadsp_create_stereo()`
保留全频左右声道 EQ 并静音低频，不改变原有 `nyadsp_create()` 的 2.1 合同。
`nyadsp_clock_process()` 为 USB 低频提供独立的队列反馈重采样。

`nyadsp_create_input_eq_split()` 在原16段输入EQ构造入口上增加可选的
`nyadsp_crossover_s`，分别配置板载LR4高通和低音LR4低通，均为40–5000Hz。
传入NULL继续使用原 `config.crossover_hz`；原配置结构、ABI版本及所有旧构造
入口不变。相同截止点与旧构造输出逐样本相同；不同截止点支持重叠或间隔，
不自动改变输出增益、延时、极性或限幅。全频立体声与安全旁路仍保持原语义。

## 产品接入与后续板测

产品接入包含 ES8388 + USB 输出生命周期、WAV 输入、设置持久化、WebUI、
双设备队列反馈时钟修正与通话优先抢占。宿主回归和产品交叉编译见音频服务文档。

尚待真实板卡验证：双设备时钟与端到端延迟、USB 热拔插、实际箱体分频和安全增益。
TTS、A2DP 和文件播放共用 `ny_dsp_output`；`nyadsp_resample` 以整数相位和48抽头窗函数 sinc 转换8–192 kHz输入至48 kHz。48 kHz输入精确直通。HFP保持原有8/16 kHz路径。

`nyadsp_create_eq(config, kinds, fullrange, error)` 为新增入口，不改变 ABI v1 配置结构、旧构造函数和离线工具合同。四个 kinds 为 OFF/PEAK/LOW_SHELF/HIGH_SHELF；架式采用 RBJ Q 参数。

16段产品设置使用 `eqBands16`，旧 `eqBands`/`eqDb` 与ABI v1结构继续保留；B54实测迁移与实时预览见音频服务文档。
