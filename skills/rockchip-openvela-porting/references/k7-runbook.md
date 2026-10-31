# K7 从源码到待验证镜像

用途：构建 K7 基线并核对镜像，不自动执行刷盘。
最小配置与打包接口依据团队仓 `0a1b7ddf0eaa1ec0469d47921fc6763024ef7c5b`；
产品准备项参考9月20日 PR #93 的 `e2aae2f858e85def2dc1fc4661534faa1685fc15`。
两者不是同一个可混装版本；后一个head有CI失败，不提供“从零全绿”保证。
其他 revision 的接口用 README、脚本 usage 与源码交叉确认。

## 环境与可追溯版本

在 Linux 构建机使用 Python 3、Git、repo、make、CMake、交叉工具链；
打包另需 dtc/fdtget、gdisk/sgdisk、dosfstools 和 rkbin 工具。
Windows 可作为串口/ADB/文件传输端。构建机路径、SSH 身份、串口号
由操作者环境指定，不继承案例机器的账户、地址或凭据。

```sh
mkdir openvela-work && cd openvela-work
repo init -u https://github.com/open-vela/contest2026_062_PharosTech \
  -b dev-ai-contest-2026 -m contest2026_062_PharosTech.xml
repo sync -c -j4
repo manifest -r -o resolved-manifest.xml
git -C contest2026_062_PharosTech rev-parse HEAD
```

上述跟踪活跃分支。复现实验时随产物保存 resolved-manifest.xml，
不能只记团队仓一个 SHA。要精确复现历史基线，应使用当时完整锁定清单；
本 Skill 未附当时所有依赖 SHA，不能保证拉当前分支得到历史相同二进制。

resolved manifest 也不记录工作区未提交修改。存在本地 patch/dirty tree 时另存
对应diff、应用顺序、目标repo版本与生成资源哈希；不要把凭据写进包。
检查团队树的 linkfile 最终落点，不能以当前shell目录代替实际源码来源。

## 先选配置

| 配置 | 用途 | 不能假定 |
|---|---|---|
| `boards/rk3576/kickpi-k7/configs/nsh` | 最小启动/串口基线 | 包含完整存储、网卡、显示；即便有nbootctl命令也未必有底层块设备 |
| `configs/dev` | 外设开发集合 | 等于最终产品、自动起Core/Eye/Web |
| `configs/core_eye` | Core与双屏 | 可以同时启用与FSPI1复用的SD插槽 |
| `configs/product` | 较晚产品分支的服务并集 | 在旧基线存在，或只换defconfig就补齐依赖 |
| `boards/.../configs/amp` | 专门的AMP启动与内存契约 | 普通NSH固件换个load地址就能用于AMP |

配置表说明用途，不要求逐个构建。优先选择能回答当前问题的最小配置。

## 最小构建与验收

在含 .repo 的工程根执行：
```sh
./build.sh contest2026_062_PharosTech/boards/rk3576/kickpi-k7/configs/nsh -j4
```

保留完整输出和退出码；确认新产物时间，避免误拿旧 nuttx.bin。
外设开发可选 `contest2026_062_PharosTech/configs/dev`；产品分支需另读下节。
并行选项随构建环境资源调整。

检查例：
```sh
aarch64-none-elf-readelf -h nuttx/nuttx
aarch64-none-elf-readelf -l nuttx/nuttx
aarch64-none-elf-objdump -h nuttx/nuttx
sha256sum nuttx/nuttx nuttx/nuttx.bin resolved-manifest.xml
```

工具名以本次实际工具链为准。检查 AArch64、入口、PT_LOAD 地址和
原始镜像开头，不把“文件存在”当链接成功；不要为了核对镜像直接运行 MMIO。

切换 Make/CMake 前隔离输出或按该 build.sh 的 distclean 语义清理构建产物，
保留源码和配置。先检查该工程的 distclean：9月19日实际发生第三方源码
解包目录被删除并触发重新下载；不能把它当“只删对象文件”的通用安全命令。
CMake 产物在配置输出目录，以构建日志定位。
NuttX 原有多仓不得为此做递归删除或清空未提交文件。

## 产品分支的额外前置条件

阅读该分支 `patches/README.md`、顶层 Makefile 和 `tools/setup_workspace.sh`。
在支持它们的分支使用正式准备入口，核对它的退出码及对应repo；不要假定
repo sync会应用团队目录里的补丁。重点检查：

- `packages/ai_agent` 扩展是否与 Core 使用的字段/函数匹配。
- nxplayer、microADB 等补丁是否进入真实 linkfile 指向的构建树。
- Eye 字体是否生成；有该脚本的分支在团队仓执行：

```sh
python3 app/nyabula/tools/generate_fonts.py --download-fallback
```

- SV6621 真机固件是否来自已授权的实际输入。公共CI的等大小全零占位只证明
  构建可通过；大小相同不是内容相同，要核对可信SHA256，不公开私有blob。
- Make与CMake都必须覆盖这些前置条件。不能把已手工补过依赖的构建机成功
  写成全新 repo 工作区也已成功；案例 E7 给出了实际反例。

## 当前 N-Boot 产品包

先读取 `tools/k7_pack/README.md`、`boards/rk3576/kickpi-k7/nboot/RELEASE`
和打包脚本，确认 `nboot-kickpi-k7.bin/.dtb`、`nboot-release.json`、
`SHA256SUMS` 齐备且对应同一release。目录为空或仍是未展开指针时，使用
该revision的release同步入口，不能临时抓latest混装。在工程根：
```sh
nuttx_image="$(realpath nuttx/nuttx.bin)"
cd contest2026_062_PharosTech/tools/k7_pack
./fetch_rkbin.sh rkbin
./build_sd.sh "$nuttx_image" \
  ../../boards/rk3576/kickpi-k7/nboot rkbin out-sd
```

先确认 nuttx/nuttx.bin 是本次 Make 生成并核验的产物。CMake 构建应将
第一行替换为对应配置输出的绝对路径，不能自动回退到旧 Make 产物。

脚本输出 `out-sd/nyabula-k7-sd.img` 是整盘发布镜像。
`build_emmc.sh` 接口相同，但产出分区包；生成包不等于获得 eMMC 写入授权。
N-Boot 固件、NuttX 裸镜像和整盘镜像不能互相替代。
官方 rkbin 下载位置并不改变其组件许可证，分发时保留许可和来源。

## 已有 N-Boot 的板上检查

仅在已有 nbootctl 和所需存储驱动的维护/产品配置中执行：
```text
nbootctl status
```

核实介质、当前域、槽位和镜像版本，再选择目标明确的 verify。
`stage` 会写入并激活非活动槽；`update-nboot` 会改启动器。
这些均是状态变更，不放入只读自检脚本。
AMP 运行时还要确认存储/外设所有权，不能让两域同时访问独占控制器。
没有有效handoff时不要靠槽优先级猜运行槽；某些已测AMP版本不发布handoff，
这时错误不等于分区已坏。最小NSH没有存储时，用启动器串口的加载/hash信息
验证启动链，不为让状态查询成功而临时扩大配置。维护操作另用 nboot-control。

板测验收：保存完整启动串口、目标镜像哈希和 build id；到达 NSH 后执行
交互命令及重启，确认实际运行的槽位/版本。编译成功只填写“编译通过”。
