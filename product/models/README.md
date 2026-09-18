# /data/models 种子

`make data` 会把本目录（`MODELS_DIR`，默认 `product/models`）整体拷入 `data.img` 的 `/models`。
模型文件体积大且不入 git，评审交付的聚合 eMMC 线刷包由构建机注入：

| 路径 | 来源 | 用途 |
|---|---|---|
| `tts/masked512.rknn`, `tts/vocoder128.rknn`, `tts/{LATENT,REF,SPEAKER,TONES,X}.BIN` | `melo-npu-20260912/models` | MeloTTS NPU 推理 |
| `face/sface.onnx` | `face-owner-20260913/models` | 主人识别 |
