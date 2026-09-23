# Nyabula 软件 DSP：阶段一

独立 C99 DSP 核心 + 调用同一动态库的 Python 离线 WAV 工具。
不修改 NuttX 的 `audio/pcm_decode.c`、ES8388、SAI 或 USB 驱动。
本阶段不注册板上应用，不改变 manifest、defconfig 或现有播放路径。

## 已实现的处理合同

- 固定 48 kHz；C API 输入为交错立体声 float32，范围 [-1, 1]。
- 输出顺序：左高通、右高通、单声道低通，交错三通道。
- 全局主增益之后，每个输入声道有最多四段 peaking EQ。
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
没有把 Windows 原生或 ARM64 板测标为通过。

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

配置仅在实例创建时生效；本阶段没有运行中参数替换或平滑切换。
不能在实时线程直接重新创建实例或修改内部系数。后续实时服务接入时，
需要控制队列和明确的过渡/淡化策略。

## 后续接入边界

1. 先接一个 ES8388 输出，验证设备生命周期和旁路。
2. 再接 USB，分别管理队列、固定延迟和采样时钟漂移。
   这里的整数延迟只补固定时间差，不是自适应重采样。
3. 再接采集/扫频工具，生成设备校准；音效预设与用户 EQ 独立于校准。
4. WebUI 只发控制请求，不在音频回调读文件或处理网络。

当前不包含混音、TTS/A2DP 输入适配、USB 输出、录音、自动校准、
声学预设、WebUI 或“真实听感已经提升”的结论。

## 算法依据

滤波器系数使用公开的双线性变换/RBJ peaking EQ 公式，自行实现：
[Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html)。
测试独立计算 LR4 的频率幅度关系，并对 EQ 中心增益、分块不变性、
左右隔离、反相合成、延迟、复位、限幅与异常文件路径做回归。
