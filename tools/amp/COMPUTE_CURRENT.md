# AMP 计算域交付边界（2026-10-17）

本文件是当前接口与所有权入口。`COMPUTE_INTEGRATION.md`、
`COMPUTE_VALIDATION.md` 保留 9 月实验过程，不再作为 Linux 摄像头服务的开发承诺。

## Linux 只接计算请求，不接管摄像头

原生 CSI/VICAP/OV5647/VEPU 已进入团队主线（#100）。采集与编码由 openvela
管理；Linux 不再启动相机、CSI/DCPHY、CIF/ISP/VPSS、SCCB I2C4 和相关 MMU，
也不认领交给原生编码路径的 VEPU。NPU、RGA 与解码器保留。

`linux/camera_ownership.py` 维护 K7 所有权清单：

- `build_kernel.sh` 对 fragment、`NYAMP_KERNEL_CONFIG` 两种入口均裁剪配置，
  在 `olddefconfig` 后复核，不允许旧实测 seed 带回相机驱动。
- 内核编译后裁剪最终 DTB；`amp_product_dtb.py` 同样处理传入的既有 DTB。
- `nboot/build_amp_fit.sh` 拒绝包含未禁用摄像头节点的 DTB。
- 此次裁剪不等于已完成 AMP 下的原生相机联调：GIC 路由、时钟/电源保持、
  Core 帧接口与实时编码须在后续相机产品集成时验证。当前 product_amp 不开启相机。

已验证的 Linux 镜像与原 DTB 保持不变。新 DTB 为独立产物，不能脱离对应
Linux 配置与 openvela 配置直接当作已验收固件发布。

## 已有接口，不改编号

| service | 当前用途 |
|---|---|
| HEALTH=1 | READY、health/info、generation |
| ASR=3 / TTS=4 / KWS=10 | 流式识别、文本合成、唤醒 |
| LLM=8 | token 生成及文本 CHAT |
| BLOB=9 | Linux 请求模型，openvela 从存储响应 |
| VISION=5 / MEDIA=6 / SPEAKER=11 | 保留编号，不能当作已交付的相机/认主/声纹服务 |

共享窗口仍为 `0x47c00000..0x48000000`（4 MiB），两端使用同一槽位头和
generation 规则。裁剪摄像头不删除语音、BLOB、tokenizer/G2P、模型会话及取消逻辑。
详细 wire contract 见 `protocol/README.md`，运行入口见 `nyampd/README.md`。

## 验证入口

```sh
python3 tools/amp/linux/test_camera_ownership.py
python3 tools/amp/test_amp_topology.py
cmake -S tools/amp/nyampd -B out/nyampd
cmake --build out/nyampd -j4
ctest --test-dir out/nyampd --output-on-failure
```

主机回放使用脚本模型，不代表真实 NPU/麦克风效果。模型权重和 vendor 库仍
不入源码 PR；准确来源与分发许可缺口继续保留在 `resources/dependencies.json`。
