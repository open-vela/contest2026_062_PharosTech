# configs — 项目功能配置

本目录存放**非 BSP 必须**的功能配置，即那些依赖外部硬件或特定项目场景的 defconfig。

## 与 `boards/.../configs/` 的区别

| | `boards/.../configs/` | `configs/`（本目录） |
|---|---|---|
| **定位** | BSP 配置（板子本身能跑的功能） | 项目功能配置（需要额外硬件或特定场景） |
| **示例** | nsh、ADB、SD 卡、emmc | 屏幕驱动、传感器扩展、创意 Demo |
| **特点** | 单靠这块板子就能用 | 通常绑定特定外设或应用场景 |

## 为什么单独放？

1. **硬件互斥**：某些外设共用引脚（如 FSPI 与 SD 卡槽），一个 defconfig 无法同时包含
2. **职责清晰**：BSP 只描述"板子有什么"（能力下限），本目录描述"我们要用这块板子做什么"（组合了 BSP 能力与项目功能的某个具体场景）
3. **便于维护**：功能配置独立管理，改 A 不影响 B

> 本目录中的每个配置通常**会组合启用 BSP 能力、测试程序与项目功能**，因此它允许超出板子最小能力边界的配置；具体每个配置的定位见各自目录下的 README。

## 使用方法（以dev为例）

```bash
# 使用本目录下的配置编译
./build.sh contest2026_062_PharosTech/configs/dev

# 或者使用cmake
./build.sh contest2026_062_PharosTech/configs/dev --cmake
```
## 注意事项

### 新增配置时务必创建 Make.defs 软链接

Makefile 模式下，`configure.sh` 会在配置目录中查找 `Make.defs`。由于本目录位于 `boards/` 之外，必须手动创建指向板级 `scripts/Make.defs` 的符号链接，否则 Makefile 模式编译会报错 `File Make.defs could not be found`。

```bash
# 在新建的配置目录中创建软链接
ln -s ../../boards/rk3576/kickpi-k7/scripts/Make.defs configs/<新配置名>/Make.defs
```

CMake 模式不需要此链接，但 Makefile 模式必须有。

### CMake 平台驱动接入

`boards/rk3576/drivers` 必须保持为指向 `../../drivers` 的有效软链。
跨 Windows/Linux 打包时使用正斜杠，不能把 Windows 的反斜杠软链目标
直接写入 Linux tar 包。断链会使 NuttX 回落到空平台 Kconfig；板级 CMake
现在拒绝这种构建，避免生成没有 LCD、RTC 和 WiFi 的产品固件。

板级 CMake 显式收录平台驱动，不依赖旧 Make 构建残留的源码树软链。
`python3 tools/tests/test_platform_cmake.py` 验证全新构建入口、重复入口去重
以及空 Kconfig 拒绝路径。
