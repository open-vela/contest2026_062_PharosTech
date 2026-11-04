# 四区灯光

在设备面板打开 **功能 → 灯光**。左耳、右耳、胸口、底座分别开关、选色和调亮度；“一键氛围”一次安排四区。灯效在设备上运行，关闭网页不会停止。

## 四个区域

| 区域 | API id | 串接位置 | 灯数 |
|---|---|---|---|
| 左耳 | `left_ear` | 第1颗 | 1 |
| 右耳 | `right_ear` | 第2颗 | 1 |
| 胸口 | `chest` | 第3–10颗 | 8 |
| 底座 | `base` | 第11–33颗 | 23 |

数据线为 K7 排针24脚（GPIO3_A1 / SPI3 MOSI），设备节点 `/dev/ws2812`。运行时通过灯光服务控制，不与直接写节点的旧测试命令并发使用。

亮度范围 **0–100%**。默认12%，开机光效也保持低亮度；开放上限不会自动把灯拉满。用户当前说明为3.3V供电，满亮供电稳定性由用户实测，不由软件上限推定。

## 灯效与预设

- 全部区域：常亮、呼吸、心跳、音乐律动。
- 耳灯：自选颜色、跟随对应眼睛瞳色、瞳色互补。互补为色相旋转180°，保留饱和度和明度。
- 胸口：额外支持状态灯——聆听蓝、思考紫、说话青绿、输入错误橙色；来自真实语音状态。
- 底座：额外支持极光渐变、环绕追光。
- 一键预设：静谧、极光、专注、随乐。预设亮度保持低档，可再逐区调整。

音乐模式读取真实 DSP 输出峰值，支持本地文件和蓝牙播放；不采集麦克风，不模拟节拍。TTS不触发音乐模式。停止或超过250ms没有新音频进度时，音乐灯区归黑。

## 开机与休眠

1. 板级初始化注册驱动后，先给整串发送黑帧，清除CPU重启前锁存的颜色。
2. Eye服务就绪后播放2.6秒低亮度开场，随后四区全黑。
3. 颜色、亮度、灯效和节奏偏好保存；**开启状态不保存、不在开机恢复**。
4. 进入实际的Eye `sleep` 状态时，耳灯、胸口、底座依次渐暗，约1.3秒归黑；唤醒后柔和恢复休眠前开启的区域。
5. 休眠中允许关灯，不允许重新点亮。全部关闭会直接提交黑帧。

## 网页、Agent与设备共用接口

固件能力标识：`core.lighting-v1`。访客可以读状态，家庭成员和主人可以设置；Agent沿用既有主人权限和执行记录。

以下示例是Agent工具的入参；网页使用 `session.request(topic, arguments)`，NSH的 `nycore product <topic> <文件>` 则读取文件中的arguments内容。

```json
{"topic":"lighting.set","arguments":{"group":"left_ear","enabled":true,"color":"#0000ff","brightness":12}}
{"topic":"lighting.set","arguments":{"group":"ears","enabled":true,"colorMode":"iris"}}
{"topic":"lighting.set","arguments":{"group":"base","enabled":true,"effect":"music","brightness":15}}
{"topic":"lighting.preset","arguments":{"name":"aurora"}}
{"topic":"lighting.set","arguments":{"group":"all","enabled":false}}
```

- `lighting.status`：四区偏好、运行阶段、输出错误、音乐来源/电平和最近提交的33像素。
- `lighting.set`：`group`可为四区id、`ears`或`all`；可选`enabled`、`color`（#RRGGBB）、`brightness`（0–100）、`brightnessDelta`（-100–100）、`effect`、`colorMode`、`speed`（1舒缓/2自然/3轻快）。不同时传绝对亮度和增量。
- `lighting.preset`：`name`为`calm`、`aurora`、`focus`或`party`。
- 写入可带最新`revision`，避免覆盖其他控制端刚做的修改；过期返回ESTALE。
- `revision`表示已接受设置版本，`appliedRevision`表示最近成功提交完整帧的版本；渐变仍可能继续。`pixels`是最近写入的灯色，不是光学传感器回读。
- `nyabula_lighting`是Agent工具。执行器等待设备提交该版本后才返回成功，不用聊天承诺代替控制。

可以在Nyabot输入，或接回麦克风后口述：

- “打开左耳灯”
- “把左耳朵的灯变成蓝色”
- “把底座灯亮度调到20%”
- “让耳朵跟随瞳色”
- “把胸口灯设为状态灯”
- “把底座灯切换为音乐律动”
- “关闭全部灯”

“提醒我关灯”“今晚十一点提醒我关灯”等延后语句保持原提醒语义，不立即切灯。

## 实现与验证

`ny_lighting_effects.c`是无硬件依赖的确定性渲染器；`ny_product_lighting.c`持有唯一设备写入线程，20Hz渲染，静态帧不重复写。固定33像素传输约1.6ms，写帧期间只阻止任务切换，不关闭硬件中断。

产品AMP配置启用 `CONFIG_NYABULA_CORE_LIGHTING=y`、`CONFIG_KICKPI_K7_WS2812=y`、`CONFIG_WS2812_CHARDEV_MAX_PIXELS=33`。Linux SPI3保持disabled。不要恢复旧SPI驱动额外÷2的分频模型。

在队伍源码目录运行主机测试；`VELA`指向完整openvela构建工作区：

```sh
VELA=/root/openvela/organize-20261017/build-workspace
python3 tools/nyabula_core/tests/lighting_test.py app/nyabula_core "$VELA/apps/netutils/cjson/cJSON"
python3 tools/nyabula_core/tests/agent_local_intent_test.py app/nyabula_core "$VELA/apps/netutils/cjson/cJSON" --fuzz 5000
```

前端新增测试纳入原`test:state`入口。2026-11-04，B99板测确认：四区映射、独立开关、瞳色同步/互补、环绕、休眠渐暗/唤醒恢复；实际Agent调用成功；本地音乐电平驱动灯效，停止后归黑。三皮肤深浅色与手机/平板/桌面布局已实际渲染检查。

本轮麦克风已由用户拆下，未做真人口播验收。USB低音炮当前未连接：音乐测试临时关闭低音炮路径并在结束后恢复原配置；日常播放前应接回低音炮或在面板关闭该开关。
