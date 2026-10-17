# Nyabula 产品配置

在已按 manifest 准备好的 openvela 工作区内构建。`configs/product` 是普通
openvela 产品固件，不是 AMP 地址布局；两种二进制不能互换。

| 配置 | openvela 入口 | 用途 |
|---|---|---|
| product | 0x40200000 | 普通固件，硬 TE，免登录 eyeprobe 关闭 |
| product_amp | 0x4a400000 | 产品计算域配套，openvela 四 A53、Linux 四 A72 |
| boards/rk3576/kickpi-k7/configs/amp | 0x4a400000 | 原有四 A53 最小 AMP 验证配置，非产品 |

AMP 配置由 `tools/generate_product_profiles.py` 从普通产品配置生成。
修改普通 defconfig 或生成器后重新生成，用 `--check` 检查是否同步。
产品 AMP 强制 SMP4（A53 MPIDR 0..3），Linux 仅保留 A72 MPIDR 0x100..0x103；
第二堆为 `0x70000000 / 256 MiB`。旧产品工作区的 UP 配置不再满足验收。蓝牙音频仍待真手机验收，产品 AMP 默认关闭它。

```sh
TEAM=contest2026_062_PharosTech
bash "$TEAM/tools/setup_workspace.sh"
python3 "$TEAM/app/nyabula/tools/generate_fonts.py" --download-fallback
# 普通构建
./build.sh "$TEAM/configs/product" --cmake -b out/product -j4
# AMP 构建：只在独立工作区应用通用 GIC 依赖
bash "$TEAM/tools/amp/nuttx/apply.sh" nuttx
./build.sh "$TEAM/configs/product_amp" --cmake -b out/product-amp -j4
```

用 `readelf -h` 核对 ELF 入口；检查展开配置中的 GIC、RAM/第二堆和 Core
语音选项。不同配置不要共享 Make 的源目录旁对象文件。构建通过不代表板测通过。

## 产品 AMP 打包

使用 `tools/amp/linux/amp_product_dtb.py` 生成产品 DTB；除了通用 AMP
窗口，还须保留 DMA 堆和第二堆。生成器同时移交 Linux 摄像头所有权。
调用 `tools/amp/nboot/build_amp_fit.sh` 时显式设置 `NYAMP_PROFILE=product`；
未设置时继续使用原有 minimal/SMP4 校验，不能静默放宽。

顶层 Makefile 的 `amp` 目标仍是最小诊断链，不是完整模型资源发行包。
完整语音镜像还需 `build_compute_initramfs.sh` 的外部运行库和模型资源；
它们不随源码提交。发行前应按资源清单补齐许可与来源。

## eyeprobe 与 UI 边界

生产固件保持硬 TE，`/eyeprobe` 关闭；认证的 `/eyes/stream` 可供面板观看。
无屏配置才开启 `/eyeprobe`：它可能展示配对信息，仅用于受控演示网络；不能
为了让无屏机器显示而更改所有生产固件的认证边界。

本轮使用 `app/nyabula_web` 的设备面板。根工作区 Web 中的 Nebula/Instrument
新皮肤和 design-lab 尚未完成设备联调，未纳入此产品配置或候选提交。
