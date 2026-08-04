# drivers

本项目下**跨板级通用**的驱动代码，存放于此。

这些驱动是 NuttX 的**内核级驱动**（lower-half driver），可以被使用相同方案、甚至不同
SoC 平台的多个板子复用（只要平台具备对应的外设总线下半边），因此源码收敛到本目录、
而非 `boards/` 下某个具体板子的目录。最终它们应当编入内核侧 `libdrivers.a`，与用户
程序（`libapps.a`）严格隔离。

## 目录层级架构

本目录（`contest2026_062_PharosTech/drivers`）通过**仓库内符号链接**
`contest2026_062_PharosTech/boards/rk3576/drivers -> ../../drivers` 暴露给内核侧
构建（详见下文「为什么会出现在 boards/rk3576/drivers」）。

```
drivers/                    # 驱动集合（源码真身，跨板级通用）
├── CMakeLists.txt          # CMake 模式构建入口：递归进入 drivers/
├── Make.defs               # Makefile 模式：include 驱动层 Make.defs
├── Kconfig                 # Kconfig 入口：if SPECIFIC_DRIVERS source 驱动层 Kconfig
├── README.md               # 本文档
├── include/                # 公共接口头文件：跨驱动共享的对内/对外接口
│   └── <driver>.h          #   仅"对外公开"的接口放此，驱动私有声明留在各驱动目录
└── drivers/                # 驱动代码根目录
    ├── CMakeLists.txt      # CMake 模式：递归进入各驱动 + 暴露 include/
    ├── Make.defs           # Makefile 模式：include 各驱动 Make.defs + 暴露 include/
    ├── Kconfig             # 驱动总开关：config NYABULA_CUSTOM_DRIVERS
    └── <driver_name>/      # 单个驱动
        ├── CMakeLists.txt  # CMake 模式：target_sources(drivers ...)
        ├── Make.defs       # Makefile 模式：CSRCS += + VPATH/DEPPATH
        ├── Kconfig         # 该驱动专属 menuconfig 选项
        └── *.c             # 驱动实现（公共头文件从 include/ 引入）
```

### 公共接口头文件目录 `include/`

`drivers/include/` 是所有驱动的**对外公共接口头文件**的统一定位：

- 驱动源码 `*.c` 用 `#include "<driver>.h"` 引入自己的公共接口；
- **内核侧调用方**（如板级 `*_appinit.c` 之类的初始化代码）也通过它调用驱动接口；
- 驱动**内部私有**的声明、结构体则放在驱动自身目录的头文件里，不对外暴露。

`include/` 目录不含构建脚本，因此不会被 CMake 的 `nuttx_add_subdirectory()` 或
Makefile 的 `include platform/.../Make.defs` 误当作驱动编译，仅作为头文件来源。
它通过驱动层的构建脚本暴露给内核侧（详见「暴露 include 给内核侧」）。

## 新增驱动

新增一个驱动，在 `drivers/drivers/` 下新建独立子目录，并按下列小节补齐文件。

### 新建目录与源文件

```
drivers/drivers/<driver_name>/
├── CMakeLists.txt          # 见「构建脚本（两种模式）」
├── Make.defs               # 见「构建脚本（两种模式）」
├── Kconfig                 # 可选：该驱动专属 menuconfig 选项
└── <driver_name>.c         # 驱动实现（公共接口头文件放 include/）
```

> 公共接口头文件放 `drivers/include/`（如 `include/<driver_name>.h`），驱动源码内
> `#include "<driver_name>.h"`；仅驱动内部使用的私有声明才留在 `<driver_name>/` 目录。

### 构建脚本（两种模式）

两种构建模式的文件都需补齐，否则该驱动在对应模式构建下不会被编译。

#### CMake 模式

```cmake
# <driver_name>/CMakeLists.txt（该文件对任意驱动通用）
# 驱动编入内核侧 libdrivers.a（drivers target），不是 apps。
if(<该驱动的使能配置项>)
  target_sources(drivers PRIVATE <driver_name>.c)
endif()
```

#### Makefile 模式：`Make.defs`（声明 CSRCS + VPATH/DEPPATH）

Makefile 模式下，编译发生在 `nuttx/drivers/` 目录（因为 `nuttx/drivers/platform` 是
指向 `boards/rk3576/drivers` 的符号链接，`nuttx/drivers/Makefile` 在
`CONFIG_SPECIFIC_DRIVERS=y` 时 `-include platform/Make.defs` 进入本目录）。因此每
个驱动的 `Make.defs` 用 `CSRCS +=` 声明源码，并用相对 `platform/` 的 `VPATH` /
`DEPPATH` 让编译系统找到 `.c` 文件：

```make
# <driver_name>/Make.defs（该文件对任意驱动通用）
# 仅当该驱动的使能配置项为 y 时才编译；配置项名由各驱动自定。
ifeq ($(CONFIG_<该驱动的使能配置项>),y)
CSRCS += <driver_name>.c

DEPPATH += --dep-path platform$(DELIM)drivers$(DELIM)<driver_name>
VPATH += :platform$(DELIM)drivers$(DELIM)<driver_name>
endif
```

### 配置接入（Kconfig 汇聚）

每个新驱动的 Kconfig 需汇聚进总入口 `drivers/drivers/Kconfig`，其配置项才会进入
menuconfig，并被总开关 `CONFIG_NYABULA_CUSTOM_DRIVERS` 统一控制。
这一步是**唯一需要手动登记**的地方（Kconfig 的 `source` 只能显式列举、无法像构建脚本那样用通配符自动发现）。

> **关键**：本目录在构建时被符号链接为 `nuttx/drivers/platform`，`nuttx/drivers/Kconfig`
> 无条件 `source "$BINDIR/drivers/platform/Kconfig"`。因此本目录内 Kconfig 的
> `source` 一律用 `$BINDIR/drivers/platform/` 前缀（与 cxd56xx/sifli 上游惯例一致），
> **不要**用相对 `../vendor/...` 之类的路径。

`drivers/drivers/Kconfig` 完整体：

```kconfig
config NYABULA_CUSTOM_DRIVERS
    bool "Nyabula Custom Drivers"
    default y
    select SPECIFIC_DRIVERS

if NYABULA_CUSTOM_DRIVERS

source "$BINDIR/drivers/platform/drivers/<driver_name>/Kconfig"

endif
```

menuconfig 中的路径为：`Device Drivers → Board Specific drivers →
Nyabula Custom Drivers`（该配置会 `select SPECIFIC_DRIVERS` 自动开启总开关）。

#### 为什么默认启用（`default y`）

改为默认启用是为保证配置简洁：

- **配置简洁**：用户无需手动开启 `SPECIFIC_DRIVERS` 与 `NYABULA_CUSTOM_DRIVERS`
  两个开关，开箱即用即可编入这些驱动。
- **无缝衔接主线**：未来这些驱动一旦合入 NuttX 主线（如迁入 `nuttx/drivers/`），
  本仓库的配置可直接平移到上游，无需再调整默认值。
- **如何屏蔽**：如果用户不需要这些驱动，只需将
  `CONFIG_NYABULA_CUSTOM_DRIVERS` 设为 `n` 即可整体关闭。

## 构建与配置机制说明

本目录通过 NuttX 官方的**板级/芯片族驱动注入通道** `CONFIG_SPECIFIC_DRIVERS` +
`drivers/platform` 编进内核侧 `libdrivers.a`：

- **`tools/Config.mk`**：`BOARD_DRIVERS_DIR ?= $(wildcard $(BOARD_DIR)/../drivers)`，
  即 `boards/rk3576/drivers`（经符号链接指向本目录真身）。
- **Makefile 模式**：构建时把 `BOARD_DRIVERS_DIR` 符号链接到 `nuttx/drivers/platform`；
  `nuttx/drivers/Makefile` 在 `CONFIG_SPECIFIC_DRIVERS=y` 时 `-include platform/Make.defs`，
  随即递归收集本目录的 `Make.defs` 与 `CSRCS`，编进 `libdrivers.a`。
- **CMake 模式**：上游在 `nuttx/CMakeLists.txt`（把 `BOARD_ABS_DIR/../drivers` 链接到
  `$BINDIR/drivers/platform` 供 Kconfig）与 `nuttx/boards/CMakeLists.txt`
  （`add_subdirectory("${NUTTX_BOARD_ABS_DIR}/../drivers" ...)` 进入本目录）两条链路上
  都已实现；本目录的 `CMakeLists.txt` 自顶向下递归，各驱动用
  `target_sources(drivers ...)` 注入已存在的 `drivers` 内核库 target。

### 驱动的自动发现

驱动层注册各驱动子目录是**全自动**的，新增驱动无需改任何注册文件：

- **Makefile 模式**：`drivers/drivers/Make.defs` 用 `include $(wildcard platform/drivers/*/Make.defs)` 通配符收集每个驱动子目录的 `Make.defs`（GNU make 的 `wildcard` 会跟随 `platform/` 符号链接遍历子目录），只要新驱动的 `Make.defs` 存在就会被自动 include。
- **CMake 模式**：`drivers/drivers/CMakeLists.txt` 通过 `nuttx_add_subdirectory()`递归进入驱动子目录（其内部 glob `*/CMakeLists.txt`），同样无需手改。

唯一的例外是 **Kconfig**：每个驱动的 `Kconfig` 仍需在 `drivers/drivers/Kconfig` 里手动 `source`（见上文「配置接入（Kconfig 汇聚）」），因为 Kconfig 的 `source` 只能显式列举、无法通配。

### 暴露 include 给内核侧

驱动的公共头文件目录 `drivers/include/` 需要暴露给内核侧编译（驱动自身与板级调用方
都在内核侧）。两种模式各配置一次、多驱动共享：

**CMake 模式** —— 在共享层 `drivers/drivers/CMakeLists.txt` 把 `include/` 追加到
NuttX 全局 include 属性（含 `drivers` 与 `board` 库都会继承）：

```cmake
if(CONFIG_NYABULA_CUSTOM_DRIVERS)
  set_property(
    TARGET nuttx
    APPEND
    PROPERTY NUTTX_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_LIST_DIR}/../include)
endif()
```

**Makefile（legacy）模式** —— 共享层 `drivers/drivers/Make.defs` 随
`nuttx/drivers/platform/Make.defs` 展开，把 `include/` 暴露给内核侧 `libdrivers.a`
编译；板级代码（`libboard.a`）是独立 make 进程，需在 board 侧 `src/Makefile` 用
`$(BOARD_DIR)/../drivers/include` 单独追加 `-I`（见 `boards/rk3576/kickpi-k7/src/Makefile`）：

```make
# drivers/drivers/Make.defs（驱动自身编译）
ifeq ($(CONFIG_NYABULA_CUSTOM_DRIVERS),y)
  CFLAGS += ${INCDIR_PREFIX}$(TOPDIR)$(DELIM)drivers$(DELIM)platform$(DELIM)include
endif
```

```make
# boards/rk3576/kickpi-k7/src/Makefile（板级调用方）
ifeq ($(CONFIG_NYABULA_CUSTOM_DRIVERS),y)
  CFLAGS += ${INCDIR_PREFIX}"$(BOARD_DIR)$(DELIM)..$(DELIM)drivers$(DELIM)include"
endif
```

> **为什么 Makefile 模式下 board 侧必须单独注入？** 因为 `libdrivers.a` 与
> `libboard.a` 是**两个互相独立的 make 进程**：驱动侧的 `drivers/drivers/Make.defs`
> 里对 `CFLAGS +=` 的修改，只会被 `nuttx/drivers/Makefile` 展开时读到，作用于驱动
> 自身 `.c` 的编译，**传不到** board 的 `make -C board`。而板级调用方
> （如板级 `*_appinit.c`）需要 `#include "<driver>.h"` 拿到驱动初始化函数的完整签名
> 与驱动公共头里定义的相关宏，所以必须在 board 侧的 `src/Makefile` 单独追加这条
> `-I`。两段注入各司其职、缺一不可：
>
> | 注入位置 | 暴露对象 | 用途 |
> |---------|---------|------|
> | `drivers/drivers/Make.defs` | `libdrivers.a` | 驱动自身 `.c` 编译 |
> | `board/src/Makefile` | `libboard.a` | 板级调用方 `#include "<driver>.h"` |
>
> 注：cxd56xx 等上游其实**没有**统一的公共 `include/` 目录可借鉴——它们的驱动头散落在
> 各驱动 `.c` 同目录，board 代码 include 的 `cxd56_gpio.h` 等是 arch/chip 侧的头，而非
> `boards/.../drivers/` 下的驱动头。我们收敛出统一的 `drivers/include/` + 两处显式
> 暴露，是更清晰的自有约定。

## 为什么会出现在 `boards/rk3576/drivers`（一个权宜取舍）

按 NuttX 的「内核/用户」分离设计，内核级驱动理应编进内核侧 `libdrivers.a`，而非
apps 侧 `libapps.a`。但要接入 NuttX 预留的内核驱动注入通道（`CONFIG_SPECIFIC_DRIVERS`
+ `drivers/platform`），`tools/Config.mk` 把 `BOARD_DRIVERS_DIR` **硬编码**为
`$(BOARD_DIR)/../drivers`，只认 `boards/<arch>/<chip>/drivers` 这个固定位置。

这带来一个两难的取舍：

- **方案 A**：把驱动挂到 `apps/vendor` 侧（经 apps 自动发现），编进 `libapps.a`。
  代价是**违反「用户/内核隔离」语义**——内核级代码按用户代码编译链接（丢失
  `__KERNEL__` 语义、头文件暴露面、库归属全部错位）。它今天之所以「能跑」，只是
  依赖「arm64 上 NuttX 的 `CONFIG_BUILD_KERNEL` 是半成品、平时根本不会进入内核模式
  构建」这一**偶然事实**；一旦哪天内核模式被修复、或换了配置，它会**静默地坏掉**，
  且症状极难排查。

- **方案 B**：把驱动符号链接到 `boards/rk3576/drivers`，走官方通道编进 `libdrivers.a`。
  代价仅是**路径上暗示的复用范围变窄**——源码真身其实仍在 `drivers/`（跨板级通用
  语义不变），只是多挂了一个仓库内链接 `boards/rk3576/drivers -> ../../drivers` 让
  构建系统能「捡到」它。

我们选择了**方案 B**，理由有三：

1. **两个「违反」不是同一量级。** 方案 A 违反的是编译期**身份正确性**（内核 vs 用户），
   会带病潜伏；方案 B 违反的只是**目录组织的暗示**（跨芯片 vs 特定芯片族），代码一个
   字节都不改、行为完全不变。
2. **符号链接几乎把方案 B 的成本降到零。** 源码不搬家、`vendor/drivers` 语义保留，
   只多一个仓库内相对链接（git 正确记录为 symlink，无需改 repo manifest）；同时
   与上游 cxd56xx/sifli 的 chip 级驱动结构**同构**，未来合主线时可直接迁到
   `nuttx/drivers/`（或 `nuttx/boards/.../drivers`），迁移路径平滑。
3. **两种违反的「保质期」不同。** 方案 B 是「放错了抽屉但贴着标签」——任何人读一眼
   目录就知道这是权宜、最终归宿是 `nuttx/drivers/`；方案 A 是「东西本身造错了」，
   隐性且无迁移信号。

> 注：`boards/rk3576/drivers` 是 **chip 族**级目录（非 `kickpi-k7` 板级），对应
> NuttX 上游「芯片族驱动」的标准位置。它把复用范围从「跨任意芯片」收窄到了
> 「rk3576 芯片族」——而我们的驱动（走 QSPI、碰 rk3576 外设）本来就属这个
> 范围，因此这其实是**更准确**而非更窄的语义。真正「跨芯片族通用」的驱动，最终归宿
> 仍是 `nuttx/drivers/`，本就该在合主线时搬离 `vendor/`。

综合以上，把驱动通过 `boards/rk3576/drivers` 符号链接接入官方 `drivers/platform`
通道、编进 `libdrivers.a`，是在「不违反用户/内核隔离」前提下、代价最小的选择；
同时保留源码真身在 `drivers/` 的跨板级复用语义，并为将来迁入 `nuttx/drivers/` 留好
了平滑出口。
