# Core / Eye / Display 验证记录

日期：2026-09-10。Core来源512956f0，Eye来源16364a3e；本次应用层基线为团队主线fb03361b。
保留原Eye表情/Scene渲染及Display的双缓冲、BlankGated、TE和FSPI调度。

## 已通过

| 门禁 | 结果 |
| --- | --- |
| K7 core_eye，CMake/Ninja，AArch64 GCC 13.4 | nuttx.bin 2340168字节 |
| K7 core_eye，Make，AArch64 GCC 13.4 | nuttx.bin 2336000字节 |
| NuttX sim，真实Core/Eye/Display主程序 | 签名JS/Wasm、13表情、25场景、字幕、撤权、lease回退通过 |
| 非法命令 | reset拒绝-13，17层嵌套拒绝-7，非有限数字/负hold_ms拒绝-34 |
| SDK | TypeScript strict类型检查、Rust wasm32 metadata编译通过；C/WAMR导入实际运行 |
| 字体 | 原字体优先/缺失回退等4项单测通过；相同输入重生成10份表，全部逐字节一致 |
| 提交 | 改动C/C++通过clang-format14；git diff --check、manifest XML、CI YAML通过 |

CMake SHA-256：`008d04a0ae90f67c8190b8e17f26ea22b58a46b3aad2519e443e393acaa8834c`

Make SHA-256：`4bbd419005325488722fd3f55d02359319b21d128ebe659fb229c7e9f2959686`

两种构建都核对了Core、Eye、Display、ST77916、FSPI、LIBCXX/ABI、签名相关配置，
不是仅构建未启用功能的NSH。Make测试树重新生成了从旧工作区复制的Kconfig缓存，
防止旧绝对路径导致Eye/Display被静默漏选；正式repo工作区不需要复制这类缓存。
构建沿用原有`.note.gnu.build-id section discarded`提示，不宣称零警告。

sim只替换物理LCD设备端点为内存ioctl接收端，仍运行生产Display主程序、调度器、
LVGL/ThorVG、Eye服务和插件Broker。服务没有mock图像或mock-ui输出。
最终输出：

```text
EYE_INTEGRATION_PASS signed-js signed-wasm captions revoke expressions=13 scenes=25 lease invalid=4 noto-fallback
```

## 构建入口

在正式repo工作区先进入队伍仓准备字体：

```sh
python3 app/nyabula/tools/test_generate_fonts.py
python3 app/nyabula/tools/generate_fonts.py --download-fallback
```

再回openvela工作区根目录，分别构建：

```sh
./build.sh contest2026_062_PharosTech/configs/core_eye --cmake
./build.sh contest2026_062_PharosTech/configs/core_eye distclean
./build.sh contest2026_062_PharosTech/configs/core_eye -j2
```

本地验证使用指定构建机的独立测试树，直接调用CMake/Ninja以及
`tools/configure.sh -a ../apps ../vendor/rockchip/configs/core_eye`和`make -j2`。
依赖源码复用正式repo的LVGL/QuickJS/WAMR/libc++等，不另外克隆单仓替代manifest。
CI从官方固定Noto版本生成字体，并随构建产物保留OFL说明和字体哈希清单。

## 正式CI补充：WAMR配置所有权

首轮正式CI（run 34448121705）中，字体生成与Make均成功，但CMake失败：

```text
<command-line>: error: "WASM_ENABLE_MODULE_INST_CONTEXT" redefined [-Werror]
```

原因是Core额外向WAMR目标注入`=0`，与正式工程的`=1`冲突。
此前独立测试树未覆盖该正式工程设置，因此本地构建通过不代表这次CI通过。
修复仅删除Core对依赖内部配置的覆盖，保留WAMR自身定义和编译器的`-Werror`。

回归用真实Core的CMakeLists构造依赖目标，分别验证上游定义为0、1时均不被Core修改：

```sh
for context in 0 1; do
  cmake -S tools/nyabula_core/tests/cmake \
    -B "out/core-wamr-$context" -DNYCORE_TEST_CONTEXT="$context"
  cmake --build "out/core-wamr-$context"
done
```

此检查只证明CMake配置所有权，不冒充完整WAMR运行测试；完整工程结果以修复后的CI为准。

## 未覆盖与边界

- 当前仅主板，双屏未连接：未验证物理左右、色序、TE/QSPI时序、帧率和长稳。
- 本次没有刷写或改动现有AMP/普通NuttX槽位。
- core_eye是独立系统配置；AMP下的LCD/IRQ所有权和接线需另行配置。
- 插件与Eye服务为内建配置，同一地址空间；不声称新增MMU隔离或跨进程Broker。
- 没有接入新的AI模型、ai_agent或Linux AI provider；未完成的能力不会静默返回mock结果。
- 字形为有限子集，任意中文输入需要增加字符集或部署完整字体。
- Display常驻运行，不提供强制kill后的清理/重启保证；恢复采用系统重启。
