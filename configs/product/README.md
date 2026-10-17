# configs/product — Nyabula 产品全功能配置

主域（A72 侧运行 Linux 时，本固件运行于 A53 簇之外的主 openvela 域；单域时独占整机）
的**交付配置**。它是以下切片的并集，并把 Core 的所有持久化目录从 `/tmp` 改到 `/data`：

| 切片 | 提供 |
|---|---|
| `configs/dev` | SV6621 WiFi/BT、SAI+ES8388 音频、USB Host/ADB、RTC/WDT/ADC |
| `configs/core_eye` | 双 GC9B72 圆屏、LVGL、Eye Engine、Nyabula Core（WAMR/QuickJS） |
| 本配置新增 | Core 产品服务（SQLite 状态、计时/闹钟、媒体、天气、简报、Web）、官方 `ai_agent`（Nyabot）、`dhcpd`（SoftAP 配网）、`webclient`、RPTUN/mailbox（AMP 计算域） |

ADB 在产品固件中**默认开启**（团队决定 2026-09-18）。

## 构建

```sh
# openvela 工作区根目录
python3 contest2026_062_PharosTech/app/nyabula/tools/generate_fonts.py --download-fallback
./build.sh contest2026_062_PharosTech/configs/product -j8
```

Makefile 模式需要 `Make.defs` 软链（已随本目录提交）。

## 与 AMP 的关系

AMP 域固件仍使用 `boards/rk3576/kickpi-k7/configs/amp`；本配置只包含主域侧的
RPTUN 链路。是否启用 AMP 由 N-Boot bootctrl 的 `amp_a/amp_b` 槽决定，主域固件不变。

## 已知取舍（2026-09-18）

- 音频留在主域（板上实测通过）；AMP 域音频尚未验证。
- BLE ISO/LE Audio/HID 未纳入，减小 ZBlue 栈占用；需要时从 `configs/dev` 取回。
- `TESTING_OSTEST/GETPRIME/EXAMPLES_HELLO` 未纳入。
