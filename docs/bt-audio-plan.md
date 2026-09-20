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
