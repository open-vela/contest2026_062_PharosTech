# OV5647分层案例与验收

## 层层缩小失败位置

| 观察 | 下一步 |
|---|---|
| I2C无ACK | 电源/复位/时钟/地址/排线；先别改ISP |
| chip ID正确但无capture节点 | sensor异步绑定、media graph、bus配置和frame interval回调 |
| STREAMON报crop大于input | 传感器物理阵列crop bounds与当前降采样输出区分 |
| DPHY拒绝启动 | lane数、bus参数、link_freq/pixel_rate合同 |
| buffer有数据但花屏 | stride、RAW10低位16bit与packed格式、Bayer顺序、字节序 |
| 帧连续却昏暗/模糊 | 曝光/镜头/照明/朝向与AWB，不把“DMA通过”写成画质通过 |

## 成功案例

K7 OV5647 R005/R010：1920×1080、RAW10低位16bit、stride3840、buffer4147200，900帧约29.387秒，`fps=30.592 sequence_gaps=0`。显式AWB改善白纸ROI色比。PC生成诊断MP4只证明可解析/预览，不是板上H.264编码。

## 反例及对焦边界

最初sensor绑定成功仍因缺get_mbus_config/frame_interval回调没有CIF节点；补齐后crop与link_freq又各自阻断。每个修复只证明新到达的阶段。

OV5647裸片资料没给出实际模组VCM接线；I2C只见0x36不能证明没装VCM。R017/R018初始化前后0x0c均无ACK，未支持“初始化把VCM弄丢”的假设。维持固定焦并明确画质边界，别编造自动对焦成功。

## 原始交付字段

保存镜像/kernel/DTB、模组/排线版本、协商format、stride/sizeimage、bytesused、sequence、timestamp、丢号计数和RAW hash。预览脚本必须写清解包/色彩/gamma步骤；证据中有人脸时沿用授权和最小采集范围，不因为调相机顺便收集生物模板。

验收：仅一个JPEG不能证明持续帧率；900帧零跳号不能证明ISP画质、AF或真实认主。
