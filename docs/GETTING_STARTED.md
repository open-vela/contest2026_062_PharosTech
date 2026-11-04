# 构建与运行

[参赛主页](../README.md) · [平台](platform/README.md) · [产品](product/README.md) · [验证](validation/README.md)

只想验证新平台，先构建NSH或一个独立外设配置；要运行Nyabula，选择普通产品或固定4+4的 `product_amp`。两种入口地址不同，不能互换二进制或启动槽。

## 获取完整工程

在Linux环境准备Git、Google repo及openvela构建依赖，保留符号链接。

```sh
mkdir nyabula-openvela && cd nyabula-openvela
repo init -u https://github.com/open-vela/contest2026_062_PharosTech \
  -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j4
```

manifest包含完整openvela基座及团队linkfile，NuttX/external/ZBlue指向配套fork。正式归档使用 `repo manifest -r` 保存多仓具体版本，同时保留额外补丁与资源清单。

## 准备产品依赖

下面命令在包含 `build.sh`、`nuttx/`、`apps/`、`packages/` 的工作区根目录执行：

```sh
TEAM=contest2026_062_PharosTech
bash "$TEAM/tools/setup_workspace.sh"
python3 "$TEAM/app/nyabula/tools/generate_fonts.py" --download-fallback
```

`setup_workspace.sh`复用manifest接线并应用产品依赖补丁，冲突时报错；字体生成器会提示所需工具/字体。不要复制其他构建树的绝对路径缓存，也不要让两个Make配置共享源目录旁对象文件。

实际WiFi/BT镜像需要合法来源的SeekWave固件。[固件说明](../drivers/drivers/sv6621/firmware/README.md)区分实物文件与公共CI零占位输入；占位输入只能验证构建，运行时无线被禁用。模型和vendor运行库也不随源码自动获得。

## 选择配置

以下路径均相对团队仓。最后一批主线CI覆盖这15套配置的CMake/Make构建。

| 路径 | 目的 |
|---|---|
| `boards/rk3576/kickpi-k7/configs/nsh` | 基础BSP与NSH |
| `boards/rk3576/kickpi-k7/configs/amp` | 最小四A53 AMP，不是完整产品 |
| `configs/dev` | 外设开发与调试 |
| `configs/display` | 双圆屏Display |
| `configs/core_eye` | Core/Eye/Display |
| `configs/core_services` | 原生服务sim验证配置 |
| `configs/hdmi` | HDMI/VOP |
| `configs/mipi_dsi` | DSI/触控 |
| `configs/camera` | CSI/VEPU/camenc独立演示 |
| `configs/ethernet` | GMAC/PHY |
| `configs/ws2812` | 灯带 |
| `configs/product` | 普通产品，入口0x40200000，硬TE |
| `configs/product_amp` | 产品4+4，openvela入口0x4a400000 |
| `configs/product_judge` | 普通产品无屏评审，软件TE/eyeprobe |
| `configs/product_amp_judge` | 4+4产品无屏评审，软件TE/eyeprobe |

普通产品示例：

```sh
./build.sh "$TEAM/configs/product" --cmake -b out/product -j4
```

AMP产品使用独立工作区并应用配套GIC补丁：

```sh
bash "$TEAM/tools/amp/nuttx/apply.sh" nuttx
./build.sh "$TEAM/configs/product_amp" --cmake -b out/product-amp -j4
```

检查生成的 `.config` 中SMP4、GIC、RAM/第二堆，并用 `readelf -h` 检查ELF入口。配置由[产品profile生成器](../tools/generate_product_profiles.py)维护，改普通defconfig后运行生成器并用 `--check`核对。

## 准备计算域与镜像

完整AMP还需要匹配的Linux Image、DTB、initramfs和N-Boot：

1. 用[AMP工具](../tools/amp/README.md)与当前所有权规则构建计算域。
2. 用 `amp_product_dtb.py` 生成产品DTB，保留共享区/第二堆并禁用Linux相机节点。
3. 动态 `nyampd` 与外部RKLLM/语音库通过 `build_compute_initramfs.sh` 收集依赖；最小静态initramfs不等于完整模型服务。
4. 产品FIT打包显式设置 `NYAMP_PROFILE=product`，携带匹配的openvela二进制与展开配置。
5. 以[资源清单](../tools/amp/resources/README.md)固定模型、tokenizer/lexicon、runtime、来源和摘要。

顶层 `make amp` 保留最小诊断链；`make product` 是产物组合工具，不会自动补齐所有外部模型与许可材料。普通SD/eMMC打包入口见[k7_pack](../tools/k7_pack/README.md)。

## 第一次运行

| 检查 | 观察点 |
|---|---|
| 启动身份 | 串口1500000 8N1；`nbootctl status`的medium/domain/slot与包一致 |
| 4+4计算域 | openvela四核与Linux四核证据；`nyampctl health`、`nyampctl info` |
| 存储与网络 | `/config`、`/data`持久化；配网成功、面板可登录 |
| 时间 | RTC/SNTP状态与实际时间 |
| 产品动作 | 改表情、音量或建立测试计时器，核对状态而非只看回复 |
| 语音与音频 | 实际说话、实际出声；按当前设备增益与输出组合验收 |
| 插件与Skill | 安装/授权一个示例；导入并启用运行时Skill |

板端可使用 `nyabula` 编号菜单或[CLI](../tools/nyabula_core/cli.md)。忘记面板密码时在有权终端执行 `nyabula panel reset-password`；原密码不可还原。不要把私钥/模型密钥写入公开配置或日志。

## 更新与恢复

运行中的设备通过配套面板或nbootctl写非运行槽，校验、激活、重启、确认；保留已知好启动器与恢复槽。N-Boot最新AMP能力仍需要配套#9—#11分支内容，不能假设任意旧release都具备相同handoff。

无屏评审配置才开放免登录eyeprobe，它可能包含配对信息，只用于受控网络。普通产品保持硬TE与认证屏幕流，不能为了预览而全局放宽认证。
