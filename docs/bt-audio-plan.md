# 蓝牙音频 / 免提 / ES8388 控制（设计备忘，2026-09-20）

状态：设计。来源为读码与 `实测日志.md` 既有条目。

## 现状

- ES8388：上游 NuttX 驱动；板级 `boards/rk3576/kickpi-k7/src/kickpi_k7_audio.c` 注册
  `/dev/audio/pcm0`（经 pcm_decode 包装）与 `pcm_in0`。两个实例共享 I2S 时钟，
  **同时收放必须同采样率同格式**。支持 8k…96k，16 bit 双声道。
  控制分两族：路由/单声道/交换/极性/麦克风静音走 `ES8388IOC_SET/GET_CONTROL`
  （按组整体 SET，要 GET-改-SET）；音量/静音/平衡/麦克风增益走 `AUDIOIOC_CONFIGURE`
  的 feature unit。Core 里此前从未调用过前者；音量只有 nxplayer 占着设备时才生效。
- SV6621 蓝牙：SCO 走 HCI（SDIO ch3），驱动代码齐，但 `CONFIG_SV6621_BT_SCO` 未开。
- ZBlue：A2DP sink/source、AVRCP CT/TG、HFP HF/AG、SCO API 齐全；A2DP 只有协商没有
  编解码。`CONFIG_BT_A2DP_SINK/SOURCE` 未开。
- SBC 编解码：`external/fluoride/fluoride/embdrv/sbc`（OI 解码器含 mSBC + 编码器），
  `CONFIG_LIB_FLUORIDE_SBC` 未开；能否脱离 fluoride 其余部分单独编是第一道关。
- openvela 蓝牙框架（frameworks/connectivity/bluetooth）在，但其 A2DP 依赖 `CONFIG_MEDIA`
  并把 SBC 交给媒体框架解码，链条大且未在本板验证——首版不用，直接基于 ZBlue。
- 已实测（协议层）：A2DP source K7→Windows、A2DP sink Windows→K7 收到真实 SBC；
  HFP HF SLC + eSCO + mSBC 协商 + SCO TX。**从未做过**：SBC→PCM→ES8388、SCO RX、
  AVRCP 数据面（需要真手机，AX211 不行）。
- 坑：NuttX 移植里的 `_net_buf_pool_list` 是手写的，新增 A2DP/SCO 缓冲池必须登记且用
  定长分配器，否则静默踩内存；ZBlue 回调里只许 memcpy，编解码放自己的线程。

## 实施笔记（随做随记，置信度见每条）

### 读码结论（2026-09-20，构建机 product-20260918 树，均为读码=仅推断，除非另注）

- ZBlue 是小米多控制器改版：公开 API 多为 `xxx_mc(dev_id,…)`，`CONFIG_BT_ORIGINAL_API=y`
  时有同名 inline 包装；HFP HF 的函数名经 `Z_API()` 包装（`CONFIG_ZBLUE_API_PREFIX_ENABLE=y`
  → 实际符号 `z_bt_hfp_hf_*`，源码里照写 `Z_API(bt_hfp_hf_register)` 或直接用宏展开名）。
- **SCO 数据面在 ZBlue 里已经有了**（openvela 改版，非上游）：`bt_hfp_hf_cb.sco_connected /
  sco_recv(data,len,packet_status) / sco_disconnected` + `bt_hfp_hf_sco_send(hf,data,len)`；
  底层 `bt_sco_send()` 用 `sco_tx_pool`（`CONFIG_BT_MAX_SCO_CONN+1` 个，K_NO_WAIT，满了返回
  -ENOMEM），`port/drivers/bluetooth/hci/h4.c` 收发都认 H4 type 0x03。应用侧无需自建缓冲池。
- A2DP sink 只收不发：媒体包走 L2CAP RX（`acl_in_pool`），**应用侧不需要新增 net_buf 池**，
  `_net_buf_pool_list` 不用动（`bt_a2dp_tx_pool` 只在 `BT_A2DP_SOURCE` 下登记）。
- `bt_sdp_init()` 已在 `bt_l2cap_br_init()` 里先于 RFCOMM/AVDTP/AVCTP/AVRCP/A2DP 调用；
  AVRCP CT/TG、HFP HF 的 SDP 记录由栈自己登记；**A2DP sink 的 SDP 记录要应用登记**
  （照 `port/.../shell/a2dp.c` 的 `a2dp_sink_attrs`）。
- 绑定持久化：当前 `.config` 里 `CONFIG_SETTINGS` 没开（defconfig 里的 `CONFIG_BT_SETTINGS=y`
  因依赖不满足被静默丢掉）→ 现在链路密钥只在 RAM。要持久化须开
  `SETTINGS + FILE_SYSTEM + SETTINGS_FILE + SETTINGS_FILE_PATH + BT_SETTINGS`，
  `bt_enable()` 后调 `settings_load()`；`port/sections/defines.c` 已登记 `bt/link_key` handler。
- **驱动侧 SCO 缺口在 NuttX，不在 SV6621 驱动**：`sv6621_bluetooth.c` 已在
  `CONFIG_SV6621_BT_SCO` 下把 H4 0x03 ↔ `BT_SCO_IN/OUT` ↔ SDIO ch3 接好；但本树
  `include/nuttx/wireless/bluetooth/bt_buf.h` 没有 `BT_SCO_IN/OUT`，`bt_hci.h` 没有
  `struct bt_hci_sco_hdr_s`，`drivers/serial/uart_bth4.c` 收发都不认 SCO（收到 `BT_SCO_IN`
  返回 -EINVAL，发 H4 0x03 走 default → -EINVAL）。补丁现成：构建机
  `/root/openvela/pr67-sco-20260913` 的三个提交（6871140c70 / 39adcab0f0 / 655bb22e09）。
- ES8388：`es8388_check_peer_format()` 要求放音与录音实例 **采样率、位宽、声道数三者都相同**，
  否则后配置的一侧 -EBUSY。驱动接受 1 或 2 声道、16 kHz。缓冲 8192 B × 4（`GETBUFFERINFO`），
  `AUDIOIOC_ALLOCBUFFER` 前必须先 `GETBUFFERINFO`（upper->nbuffers 由它填）。录音侧一次收满
  `nmaxbytes`，所以通话要自己按小尺寸 `ALLOCBUFFER`（否则 16k 单声道一块 = 256 ms）。
- `/dev/audio/pcm0` = `pcm_decode` 包在 ES8388 前面：首块必须以 44 字节 WAV 头开始，头里的
  data 长度不被跟踪（一直播到 `AUDIO_APB_FINAL`）→ 流式输出 = 先喂一个合成 WAV 头。板上没有
  注册裸 codec 输出节点；`pcm_in0` 是裸 ES8388 录音节点。
- SBC：`external/libfluoride-sbc`（独立包，`CONFIG_LIB_FLUORIDE_SBC`，Apache-2.0，OI 解码器含
  mSBC + 编码器含 `SBC_FORMAT_MSBC`），不依赖 fluoride 其余部分。构建机 ffmpeg 带 `sbc` 编码器，
  可生成测试向量。

### 进度

- [x] `app/nyabula_core/ny_sbc.{c,h}`：SBC/mSBC 解码 + mSBC 编码 + H2 成帧/拆帧的窄接口。
  坑两个（主机测试抓到）：①库的 `OI_CODEC_SBC_SkipFrame` 遇 CRC 错也拒绝跳帧，得自己按帧头
  算长度跳；②`SBC_Encoder_Init` 会按 `u16BitRate` 重算 bitpool，mSBC 必须在 Init **之后**
  再写 `s16BitPool=26`，否则编码输出不是 57 字节。
- [x] `app/nyabula_core/ny_pcm.{c,h}`：字节环/记录环、合成 WAV 头、单双声道、RMS、半双工门控。
- [x] 主机测试 `tools/nyabula_core/tests/bt_audio_test.{py,c}` + `bt_audio_vectors.inc`
  （ffmpeg 7.1.5 生成的 1 kHz 正弦：SBC 44.1k 立体声 6 帧 + mSBC 4 帧）。ASan+UBSan 下
  PASS（置信：主机实测）。运行：
  `python3 tools/nyabula_core/tests/bt_audio_test.py app/nyabula_core <openvela>/external/libfluoride-sbc/libfluoride-sbc`

- [x] `ny_audio_stream.{c,h}`：NuttX 音频节点的流式收放（mq + apb，自带合成 WAV 头、小块
  ALLOCBUFFER）。交叉编译通过（`/root/openvela/bt-work/cc.sh`，真实 flags 含 -Werror）。
- [x] `ny_product_bt_audio.{c,h}`：音频线程（A2DP 抖动缓冲→SBC 解码→pcm0；通话 SCO↔mSBC/CVSD
  ↔pcm0/pcm_in0、半双工门控、RX 驱动 TX 节拍 + RX 静默时按时钟补发）、扬声器仲裁
  `ny_bt_audio_hold/release`。不含任何蓝牙头文件。待交叉编译。
- [x] `ny_product_bt.h` 公共头。
- [x] `ny_product_bt.c`：栈拉起线程（先等 `/dev/ttyHCI0`，没有控制器就 bt_enable 会 assert）、
  可连接常开/可发现限时窗口、Just Works + 面板确认、设备表（store 域 `bluetooth`）、全部
  `bt.*` 主题、A2DP sink 端点 + SDP 记录、AVRCP CT（播放控制/曲目/播放状态）+ TG（绝对音量
  双向、无绝对音量手机的音量键）、HFP HF（SLC/来电/接听/拒接/挂断/CLIP/RING/VGS/编解码协商/
  SCO 收发）。交叉编译通过（带/不带 BT_SETTINGS 两种）。
- [x] media.c 仲裁钩子（claim/延后 alert/空闲 release/`music.status.source`）、store 白名单、
  ny_product.{c,h}、ny_product_runtime.c 接线。BT=y 与 BT=n 两种都交叉编译通过
  （runtime.c 第 154 行 `nbootctl_handoff_read` 参数个数报错是别人的改动对不上构建树里旧版
  nbootctl，与蓝牙无关，BT 开关都一样报）。
- [x] Kconfig/Makefile/CMakeLists（重读后脚本化外科式插入，CRLF 保持；CMake 分支**未验证**，
  产品只走 make）。
- [x] `patches/nuttx-bluetooth-sco.patch` + README 行；在 scratch 副本上打补丁后
  `uart_bth4.c` 与 `sv6621_bluetooth.c -DCONFIG_SV6621_BT_SCO` 均编过。
- [x] defconfig 片段经 `olddefconfig`（scratch 的 .config 副本）验证全部符号存活；受影响的
  ZBlue 文件（a2dp.c/keys_br.c/settings*.c/fs.c/defines.c）带新选项交叉编译通过。

已确认的接口事实（读码）：zephyr 头与 NuttX/cJSON 头同文件共存，在产品 flags
（-Wshadow -Wundef -Werror）下可编过；AVRCP 发命令传 `pool=NULL` 走栈内 `acl_tx_pool`，
应用侧零自建 net_buf 池；SSP 在 `CONFIG_HCI_AUTO_REPLY_IN_JUST_WORK=y` 下 Just Works 走
`pairing_confirm` 回调，否则走 `passkey_confirm(conn, 0xFFFFFFFF)`，两个都要接；
`CONFIG_BT_MAX_PAIRED=1` 只能绑一台手机，要调大。

## 落地顺序

1. 单开 `LIB_FLUORIDE_SBC` 验证可独立编译（不行就把 embdrv/sbc 十几个 C 文件带许可
   声明收进 `app/nyabula_core/sbc/`）。
2. `ny_product_audio.c`：`audio.status / output.route / volume / input.route / mic.gain /
   mic.mute / channel`，直接作用于编解码器 fd 并持久化；`music.volume` 转发到同一路径；
   `audioctl` 补 volume/mute/mic gain；面板"音频路由"接上。
3. `ny_sbc.c` 解码封装 + 离线样本（`tmp/sbc-silence*.sbc`）→ pcm0 验证。
4. `ny_product_bt.c`：`bt_enable`、**先** `bt_sdp_init()` 再注册 profile、可发现/可连接、
   数字比较配对、`bt.*` 主题、绑定持久化（/data/settings）。
5. A2DP sink：SBC SEP + 定长缓冲池；`recv` → 环形缓冲 → 解码线程 → pcm0（44.1k 立体声，
   无需重采样）；抖动缓冲 200–300 ms。面板音乐源"蓝牙"。
6. AVRCP CT：播放控制、曲目信息、绝对音量（需真手机）。
7. 开 `SV6621_BT_SCO`，回归 HFP SCO TX，再打通从未跑过的 SCO RX。
8. `ny_product_call.c`：来电/接听/挂断状态机；通话时把 pcm0/pcm_in0 **同时改开成
   16 kHz 单声道**（mSBC 免重采样），7.5 ms 帧、约 30 ms 小缓冲，结束后恢复 44.1k。
   优先协商 mSBC；CVSD 取决于控制器 Voice Setting，未验证。
9. 回声：首版用"扬声器有输出时麦克风门控"，AEC 后续。面板"通话"从规划中转为已实现。
10. 可选：A2DP source（设备 → 蓝牙音箱），复用编码器。

> 与上面顺序的出入：第 8 步没有单独的 `ny_product_call.c`，通话状态机在 `ny_product_bt.c`，
> 通话音频在 `ny_product_bt_audio.c`；配对用 Just Works + 面板确认（任务要求），不是数字比较。

## 已落地的实现（2026-09-20）

### 文件

| 文件 | 作用 | 验证 |
|---|---|---|
| `app/nyabula_core/ny_sbc.{c,h}` | SBC/mSBC 窄接口 + H2 成帧 | 主机测试 PASS；交叉编译 |
| `app/nyabula_core/ny_pcm.{c,h}` | 环、WAV 头、声道、RMS、半双工门控 | 主机测试 PASS；交叉编译 |
| `app/nyabula_core/ny_audio_stream.{c,h}` | NuttX 音频节点流式收放 | 交叉编译 |
| `app/nyabula_core/ny_product_bt_audio.{c,h}` | 音频线程、抖动缓冲、通话音频、扬声器仲裁 | 交叉编译 |
| `app/nyabula_core/ny_product_bt.{c,h}` | 栈/配对/主题/A2DP/AVRCP/HFP 信令 | 交叉编译（符号逐个对过 ZBlue 源码） |
| `patches/nuttx-bluetooth-sco.patch` | NuttX 侧 SCO 三处缺口 | scratch 副本打补丁后编过 |
| `tools/nyabula_core/tests/bt_audio_test.{py,c}`、`bt_audio_vectors.inc` | 主机单测 | ASan+UBSan PASS |

### defconfig 片段（不改 `configs/product/defconfig`，由负责人合入）

```
# 蓝牙音频：必需
CONFIG_BT_A2DP_SINK=y
CONFIG_LIB_FLUORIDE_SBC=y
CONFIG_NYABULA_CORE_BT=y
# 通话音频：必需，且要先给 nuttx 打 patches/nuttx-bluetooth-sco.patch，否则编不过
CONFIG_SV6621_BT_SCO=y
# 手机重启后还认得：必需（defconfig 里已有的 CONFIG_BT_SETTINGS=y 因缺下面三项一直被静默丢掉）
CONFIG_SETTINGS=y
CONFIG_FILE_SYSTEM=y
CONFIG_SETTINGS_FILE=y
CONFIG_SETTINGS_FILE_PATH="/data/settings/bt"
CONFIG_BT_SETTINGS=y
# 多于一台手机
CONFIG_BT_MAX_PAIRED=4
CONFIG_BT_KEYS_OVERWRITE_OLDEST=y
# 查询应答（EIR）里带产品名，而不是 "Zephyr"
CONFIG_BT_DEVICE_NAME_DYNAMIC=y
CONFIG_BT_DEVICE_NAME_MAX=33
# 可选：栈自己的静态 CoD（运行时反正会被 NYABULA_CORE_BT_COD 覆盖）
CONFIG_BT_COD=0x240414
```

已在 .config 里、无需再动：`BT_CLASSIC BT_A2DP BT_AVDTP BT_AVRCP BT_AVRCP_CONTROLLER
BT_AVRCP_TARGET BT_HFP_HF BT_HFP_HF_CODEC_NEG BT_HFP_HF_CODEC_MSBC BT_HFP_HF_VOLUME
BT_HFP_HF_CLI BT_RFCOMM BT_MAX_SCO_CONN=1 BT_MAX_CONN=2 HCI_AUTO_REPLY_IN_JUST_WORK`。
`NYABULA_CORE_BT_*` 的其余项都有默认值（见 Kconfig）。`olddefconfig` 验证：上述符号全部存活。

### 代价估算

- Flash：本服务 .text 约 49.5 KB + .data 3.5 KB（aarch64 -Os 实测目标文件：bt 32.6K、
  bt_audio 11.4K、audio_stream 2.8K、pcm 1.5K、sbc 1.2K）；libfluoride-sbc 16.4 KB；
  ZBlue settings + fs 约 6.5 KB；A2DP sink 分支与 SV6621 SCO 分支各 <1 KB。合计约 **75 KB**。
- RAM：媒体环 96 KB + SCO 环 4 KB + 麦克风/发送环 8.5 KB + 两个解码器和一个编码器约 10 KB
  + 音频线程栈 16 KB + 音频块（音乐 4×4.1 KB 或通话 6×0.7+4×0.6 KB）+ .bss 约 5 KB，
  拉起线程栈 8 KB 用完即还。稳态约 **140 KB，放音乐时峰值约 160 KB**。
- CPU（仅推断）：44.1 kHz 立体声 SBC 定点解码在 A72 上远低于一个核的 5%。

### 线程与所有权

- ZBlue 回调：只在 `g_bt.lock` 下记状态、置 work 位；音频只 memcpy 进环 + `sem_post`。
- `nybtaudio` 线程（优先级 120）：解码/编码/一切音频设备调用。等待用 `sem_clockwait(MONOTONIC)`
  ——墙钟会被新加的 SNTP 服务拨动，不能拿来等。
- 产品 worker 的 `ny_product_bt_tick()`：回调推迟的所有事；AVRCP 每 tick 最多发一条命令。
- 扬声器一个主人：`music.play`/铃声在 `ny_media_start()` 里先 `ny_product_bt_speaker_claim()`
  （蓝牙线程关设备后才返回，最多 600 ms；放歌时顺带 AVRCP PAUSE，铃声不暂停手机、响完自动回来）；
  播放器回到空闲且没有待响的 alert 时 release。A2DP 开始播 → `ny_product_media_sleep()` 停
  flash 音乐（不停铃声）。**通话不让位**：通话中 claim 返回 -EBUSY，alert 不消耗、挂断后再响。
  `music.status.source` = `flash|alert|bluetooth|call|none`，`bt.status.audio.owner` 同源。

## 上板测试脚本（真手机；逐步，预期输出写在每步后）

前置：nuttx 已打 SCO 补丁；defconfig 片段已合；固件带热更新件。串口另开日志。
所有主题都可从 nsh 发：`echo '{}' > /tmp/e.json`，然后 `nycore product <topic> /tmp/e.json`，
输出一行 `PRODUCT_RESULT {...}`；面板 WebSocket 发同名主题等价。日志前缀 `nybt:`。

0. **启动**。开机后 10 s 内日志应有 `nybt: ready`。
   `nycore product bt.status /tmp/e.json` → `"state":"ready"`，`name` = 产品名，`address`
   非空，`discoverable:false`，`bondsPersistent:true`。
   - `state` 一直 `starting`：`ls /dev/ttyHCI0` 不存在 → 控制器没起（板级 BT 启动失败）。
   - `failed` + `error:-19`：等 120 s 没等到 HCI 节点；其它负值 = `bt_enable`/profile 注册返回值。
1. **陌生手机此时搜不到**（不可发现）。手机蓝牙列表里不应出现产品名。
2. **开配对窗口**：`echo '{"enabled":true,"seconds":120}' > /tmp/d.json`；
   `nycore product bt.discoverable /tmp/d.json` → `discoverable:true, discoverableSeconds≈120`；
   日志 `nybt: pairing window open`。手机上应出现产品名、音箱图标。名字若是 "Zephyr" =
   `BT_DEVICE_NAME_DYNAMIC` 没开。
3. **配对**：手机点它。面板/眼睛应出现通知 `Bluetooth pairing request: AA:BB:..`；
   `bt.status` 出现 `"pairing":{"address":..,"secondsLeft":≤25}`。
   `echo '{"accept":true}' > /tmp/p.json`；`nycore product bt.pair.confirm /tmp/p.json`
   → 日志 `nybt: paired AA:.. bonded=1`，随后 `nybt: connected`、`a2dp connected`、
   `avrcp controller connected`、`hfp service level connection up`。
   - 25 s 不确认 → 手机提示配对失败（预期）。窗口关着时配对 → 日志
     `pairing refused: not in pairing mode`（预期）。
   - 若根本没有回调、手机直接配上：说明栈在 Just Works 下自动应答了——记下来，需要查
     `ssp.c` 的 `BT_CONN_BR_PAIRING_INITIATOR` 分支。
4. **设备表**：`nycore product bt.devices /tmp/e.json` → 该手机 `bonded:true, connected:true`，
   几秒后 `name` 为手机名。
5. **A2DP**：手机放歌。日志 `nybt: a2dp streaming`；约 200 ms 后出声。
   `bt.status`：`a2dp.state:"streaming", codec:"SBC", sampleRate:44100|48000`，
   `audio.owner:"bluetooth"`，`mediaPackets/mediaFrames` 持续涨，`mediaBadFrames:0`，
   `mediaUnderruns` 起步 0（偶发 +1 可接受），`bufferMs` 在 100–400 之间晃。
   `music.status` → `"source":"bluetooth"`。
   - 没声但计数在涨、`audio.error:-16`：pcm0 被别人占着；`-22`：pcm_decode 没认合成 WAV 头。
   - 有声但每秒卡：看 `mediaUnderruns`（涨 = 无线/调度，先把 PREBUFFER 调到 400 再测）
     还是 `mediaDropped`（涨 = 环满，解码线程被饿）。同时跑 WiFi iperf 复测一遍（共存）。
   - 暂停 → 日志 `a2dp suspended`，`audio.owner:"none"`（设备已还给 flash 播放器）。
6. **AVRCP 控制**：`nycore product bt.media.pause /tmp/e.json`、`bt.media.play`、
   `bt.media.next`、`bt.media.prev` → 手机播放器相应动作；`bt.status.avrcp.playing` 跟着变，
   切歌后 1 s 内 `title/artist` 更新（iOS/多数安卓播放器都给；没给就是空，不算失败）。
7. **绝对音量**：手机音量键 → `audio.status` 的 volume 跟着变（≈ 手机值/127×100）；
   面板改 `music.volume` → 手机音量条跟着动。`avrcp.absoluteVolume:true`。
   - 手机条不动：手机没注册 VOLUME_CHANGED（老安卓要在开发者选项里开"绝对音量"）。
8. **仲裁**：蓝牙放歌时 `music.play` 一首 flash 歌 → 蓝牙静音、手机被暂停、flash 出声，
   `source:"flash"`，`bt.status.audio.held:true`；flash 播完/`music.stop` 后 `held:false`，
   手机再按播放即恢复。蓝牙放歌时让一个倒计时到点 → 铃声盖过音乐（手机不暂停），响完音乐回来。
9. **HFP 来电**：用另一部手机拨入。日志 `nybt: call incoming`；通知 `Incoming call: <号码>`；
   `bt.status.hfp`：`call:"incoming", number, inbandRing`。带内铃声的手机此时就有
   `call audio up, mSBC`；不带的每次 RING 响一声本机铃。
   `nycore product bt.call.answer /tmp/e.json` → `call active`；
   `hfp.codec:"mSBC", audio:true`，`audio.owner:"call"`，`sampleRate:16000`，
   `scoRxPackets` 与 `scoTxPackets` 都约 133/s，`scoTxFailed:0`，`scoRxBad` 近 0。
   双向通话试听；对方说话时 `micGated:true`（对方不应听到自己的回声）。
   - **`scoRxPackets` 为 0 而 Tx 在涨** = 本次最大的未知数（SCO RX 从未跑通过）：先确认补丁
     已打（`uart_bth4` 否则丢包）、`SV6621_BT_SCO=y`；再查 SDIO ch3 是否有上行
     （`sv6621_stats`）；再怀疑控制器把 SCO 路由到了 PCM 脚而不是 HCI（需厂商 HCI 命令）。
   - `audio.error:-16` 且无声：ES8388 拒绝 16 kHz 单声道与另一方向不一致——看是否自动退到
     了立体声（`channels:2`）；`-22`：SAI 不支持该采样率。
   - 声音断续：`scoTxFailed` 涨 = `sco_tx_pool` 只有 2 个缓冲，发送比控制器消化快。
   `bt.call.hangup` → `call idle`、`call audio down`，`audio.owner` 回 `none`，
   之后 A2DP 可立即恢复。来电时 `bt.call.reject` → 手机侧显示已拒接。
10. **CVSD 回退**：找一台不支持 mSBC 的手机（或安卓开发者选项关 HD Voice）→
    `hfp.codec:"CVSD"`，`sampleRate:8000`，其余同上。
11. **持久化**：重启。`bt.devices` 仍 `bonded:true`；约 8 s 后本机主动回连一次
    （日志 `connected`）；手机不需重新配对。`/data/settings/bt` 存在。
12. **忘记**：`echo '{"address":"AA:BB:CC:DD:EE:FF"}' > /tmp/f.json`；
    `nycore product bt.forget /tmp/f.json` → 断开、`bt.devices` 里消失；手机再连会要求重新配对。

每步的原文输出请按 CLAUDE.md 格式记进 `实测日志.md`，把下面"需上板验证"的条目升级置信度。

## 需上板验证的假设 / 风险

1. **SCO RX 走 HCI**：驱动与栈的代码路径都在，但从未见过一个上行 SCO 包；控制器默认 SCO
   路由未知。发送侧已做"RX 静默则按时钟补发"，所以即便 RX 不通，对方仍能听到我们，便于二分。
2. **SV6621 WiFi/BT 共存**：同一 SDIO 功能口、同一 rx 线程。A2DP 约 330 kbit/s + WiFi 大流量时
   的抖动未知；旋钮是 `NYABULA_CORE_BT_A2DP_PREBUFFER_MS` 与 `SV6621_RXPRIO`。通话 7.5 ms 周期
   对 rx 路径的延迟很敏感：现驱动是中断驱动（读码，`kickpi_k7_sv6621_transport.c` attach_irq），
   没有旧 skw 驱动那种 20 ms 轮询；若 `scoRxPackets` 仍成簇到达，查 rx 线程优先级与 WiFi 大包占用。
3. **ES8388/SAI 的 16 kHz（与 8 kHz）单声道**：驱动接受，板上没跑过；代码会自动退立体声。
   收放必须同格式，通话期间任何别的录音用户（将来的语音唤醒）会互相 -EBUSY。
4. **小块连续 I2S 发送**：通话 20 ms 一块、`ES8388_INFLIGHT=2`，块间是否无缝未验证；
   有"哒哒"声就把 `NY_BT_CALL_CHUNK_MS` 调到 40。
5. **pcm_decode 吃合成 WAV 头**（长度 0xFFFFFFFF）：读码成立，未上板。
6. **ZBlue 多控制器改版 + BT_SETTINGS + 文件后端**在本板从未开过；`settings_load()` 之后身份地址
   是否稳定、`/data` 未挂载时的行为未知（失败只记日志，不阻塞启动）。
7. **Just Works 回调**确实在被动配对时触发（否则"面板确认"形同虚设，见测试第 3 步）。
8. **时钟漂移**：没有重采样。手机比本机快 → 环满丢一包（约 20 ms）；慢 → 欠载后重新缓冲。
   50 ppm 量级下是几分钟到几十分钟一次的轻微瑕疵。
9. **本机主动回连只开 A2DP/AVRCP**，HFP 由手机跟进（主流手机如此）。主动连 HFP 需要 SDP 查询，
   而 SDP 客户端要一个应用自建 net_buf 池——正是 `_net_buf_pool_list` 那个坑，所以有意不做。
10. `bt_conn` 失败回调：若栈对失败的外呼不回调 `connected(err)`，30 s 后 tick 自行放弃。
11. CMake 构建分支未验证；`.github/workflows/build.yml` 未接入新的主机测试。
