# Eye scene 字段契约

Eye Engine 只接受预定义 scene 和 typed payload。Nyabula Core、蓝牙、Flutter、
MCP 与 LLM 都不得直接创建 LVGL 控件或逐帧修改绘制参数。

## 生命周期

1. `nyabula_eye_engine_show_scene()`：从当前眼皮状态闭眼，闭合后原子替换内容；
2. 全屏模式重新睁眼后显示 scene，极简模式让 scene 留在闭合的黑色眼皮上；
3. `nyabula_eye_engine_update_scene()`：复制新 payload，并在 320 ms 内交叉淡入；
4. `nyabula_eye_engine_hide_scene()`：全屏模式闭眼清场再睁眼；极简模式直接睁眼，
   内容受上下眼皮并集蒙版裁剪并同步淡出；
5. 所有时间均使用 `lv_tick_get()` 单调时钟采样，掉帧不会改变动画时长。

## 通用字段

`title`、`subtitle`、`detail`、`value` 是短文本；`previous_line`、
`current_line`、`next_line` 用于歌词、字幕等三行内容。`duration_ms`、
`position_ms`、`remaining_ms`、`elapsed_ms` 使用毫秒。`progress` 使用 0～1。
布尔字段包括 `active`、`playing`、摄像头/麦克风隐私状态和信号质量。

## Scene 与字段

| Scene | 主要字段 |
|---|---|
| music | `music_view`、播放状态、曲名、时长/位置、三行歌词 |
| timer | 总时长、剩余时间 |
| weather | 六种天气、温度/体感、湿度、风速、能见度 |
| battery | 五种电池状态、电量、预计时间与提示 |
| alarm | 时分、名称/提醒/隐藏模式 |
| call | 来电/通话/结束、联系人、号码、通话时长 |
| task | 执行/排队/确认/成功/失败、标题、详情、进度 |
| stopwatch | 已用时间、运行状态 |
| calendar | 年月日、日程标题与详情 |
| sleep timer | 剩余时间、播放淡出状态 |
| network | Wi-Fi/蓝牙/离线、网络名与连接说明 |
| audio | 扬声器/耳机/同时输出/静音、音量与路由说明 |
| eq | 配置/自动校准、配置名、十段增益、校准进度 |
| caption | 前一行、当前行、后一行字幕 |
| briefing | 当前条目、条目总数、标题与朗读状态 |
| privacy | 摄像头、麦克风占用和本地处理说明 |
| identity | 主人名称、确认状态与模板存储说明 |
| memory | 待记忆内容、确认状态 |
| devices | 在线设备数量和端点说明 |
| system | openvela、AMP、NPU 与健康状态 |
| health | 心率、信号质量和免责声明 |
| presence | 距离、注视/在场状态 |
| companion | 陪伴模式标题、提醒策略 |
| home | 猫舍时间、天气和视窗说明 |
| subwoofer | 2.1 状态、分频点与低音单元在线状态 |

## 资源生成

- `tools/generate_vector_icons.py` 将已批准 SVG 的 move/line/quadratic/cubic/close
  命令无损固化为 C 资源；renderer 直接交给 ThorVG 绘制原始 Bézier 路径。路径
  reveal 仅在过渡期间生成抗锯齿折线，最终填充与描边保持原 SVG 曲线。
- `tools/generate_fonts.py` 从 Web Demo 实际文案提取字形，生成 4bpp、未压缩的
  LVGL 字体。标题使用阿里妈妈数黑体，正文使用 MiSans Semibold；英文数字使用
  开源、与 Times New Roman 度量兼容的 Tinos Bold，避免分发微软专有字体。
- 新增产品文案后必须重新运行字体生成器，否则未知字形由上层文案校验拒绝，
  不能静默显示方框。
