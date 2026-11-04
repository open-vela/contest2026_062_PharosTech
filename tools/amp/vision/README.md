# 身份与本地视觉

在“身份”页开启身份识别，输入称呼后主动登记人脸；下方“认出你的声音”登记主人声纹。声纹需要说四次当前唤醒词，登记后可先测试，再选择“仅回应主人”。该开关默认关闭。

在“摄影”页可开启人脸检测。物品学习入口本批默认隐藏，语音入口和产品API也由默认关闭的 `CONFIG_NYABULA_CORE_OBJECT_TRACKING` 控制；代码和主机测试保留供后续开发。B109实机仍在RKNN加载阶段失败，不能按可用功能验收。

五点校准将相机坐标映射到屏幕猫眼；未校准时可以看检测／追踪框，不提供伪造的注视坐标。这里只控制屏幕眼睛，不控制实体舵机。

## 运行边界

- openvela 独占采集、编码与产品控制；Linux 只接收原始帧，运行 NPU 和后处理。
- 身份、声纹与物品特征留在设备。主动调用独立的 Agent `camera_vision` 工具时，需经过批准才能向配置的 MiMo 发送照片。
- 人脸登记需主动输入称呼；声纹需本人主动登记。两者用于陪伴交互，不作为面板登录凭证。
- 离开网页不会停止已经主动开启的视觉分析；点“停止分析”或关闭摄像头隐私开关结束。
- 物品模板只用于本次追踪，最多四个视角，停止后清除。它不是永久物品库，也不做通用物品类别识别。
- 健康／rPPG 已从本批范围移除。

## 设备文件与协议

|用途|设备路径|
|---|---|
|人脸检测|`/data/models/face/detector/yunet.rknn`|
|人脸特征|`/data/models/face/recognizer/sface.rknn`|
|物品追踪|`/data/models/vision/tracker/{template,search,head}.rknn`|
|主人声纹|`/data/models/speaker/model.onnx`|

每个模型目录保留原模型许可和来源。模型通过既有 BLOB 服务送入 Linux tmpfs，不另开下载服务。旧的 `/data/models/face/sface.onnx` 源文件可保留，但身份推理使用转换后的 RKNN。

VISION 服务编号 5，协议在 `../protocol/nyamp_vision_wire.h`。VGA NV12 共享槽为 offset `0x160000`、size `1 MiB`，原语音与 BLOB 地址不变。BEGIN 取得代际绑定租约；每次仅一帧在途，异步回复后才能复用。计算端复制帧后推理，END 取消结果并释放租约。

控制主题仅 owner 可用：
- `vision.start {"follow":false}`：人脸检测；追加 `"identify":true` 开启身份特征。
- `vision.learn {"follow":true}`：中央 160×160 框学习；可传归一化的 `x/y/width/height`。
- `vision.status {}`：阶段、帧数、错误、结果年龄、脸框或物品目标。
- `vision.stop {}`：停止分析、释放相机订阅、清除物品模板。

关闭摄像头隐私会停止视觉并拒绝新帧。网页隐藏超过 700 ms 的结果，猫眼也不使用过期结果。物品学习跳过启动前 32 帧；学习阶段至少累计 3 秒且 8 个可信帧。低置信或外观不符时进入 lost，冻结模板并暂停跟随；连续三帧位置一致、置信与外观均满足条件后恢复。

## 模型来源与处理

YuNet2023mar 与 SFace 来自 OpenCV Zoo 固定提交 `48b10fa505aeb51fbd127fc60a5c660040e823b7`。YuNet 输入为 limited-range BT.601 NV12 转 BGR uint8，补黑至 640×640，不拉伸 VGA 原图。检测阈值 0.7、NMS IoU 0.3、最多五张脸。SFace 使用五点对齐，输出归一化 128 维特征。

NanoTrack v2 来自作者仓固定提交 `248663fde6bf7c40190cf10ee396d5662919ecd3`。模板输入127、搜索输入255，特征分别48×8×8、48×16×16；head输出2×16×16分类与4×16×16边距。后处理依据该版本配置：stride16、window0.490、penalty0.150、LR0.385，不混用默认配置或其他版本参数。RKNN Toolkit2 2.3.2，目标rk3576，非量化。

参考：[YuNet](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/face_detection_yunet)、[NanoTrack v2配置](https://github.com/HonglinChu/SiamTrackers/blob/248663fde6bf7c40190cf10ee396d5662919ecd3/NanoTrack/models/config/configv2.yaml)。

## 已有证据与剩余验收

- B106真实人脸检测样本14帧／1249 ms（11.21帧／秒），停止和隐私关闭后回到idle、readers=0；用户确认真人框位置和移动正常。
- 人脸数学、共享租约、旧代际、异步复制、取消、身份特征、物品学习与三帧找回的主机回归通过。
- Core学习链的owner/隐私/ROI/启动窗口/丢失不驱动猫眼/清理测试，普通及ASan/UBSan通过。语音入口意图测试通过。
- 声纹登记、匹配、取消、删除、存储与默认关闭门控已有主机测试；声纹模型已安装。
- B108身份模型已连续处理真实帧，用户已完成人脸登记；声纹模型已ready，真人效果按用户要求跳过验收。B109物品追踪仍失败，入口默认关闭；实物转面、遮挡恢复和猫眼标定效果仍未验收。
- Cloud远程画面、双向对讲和经批准的MiMo看图还需真实联调。
