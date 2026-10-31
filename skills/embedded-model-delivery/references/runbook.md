# 清单、路径及K7案例

## 最小清单

保留 `schema_version、bundle_id、version、kind、compatibility、provenance、redistribution、license_files、files`。
compatibility需SoC/architecture/OS/interface版本与runtime精确依赖；provenance为source/revision/conversion；每个file为包内相对path、bytes、sha256。版本不用latest。

项目 `nyamp_bundle.py` 支持本地verify与确定性pack，不联网、不安装、不写盘。拿到匹配源码后可在已有工程执行：

```sh
python3 tools/amp/resources/nyamp_bundle.py verify /path/to/staging --profile /path/to/target-profile.json
python3 tools/amp/resources/nyamp_bundle.py pack /path/to/staging --profile /path/to/target-profile.json --output /path/to/model-v1.tar.gz
```

路径由实际staging/profile替换。profile来自目标固件；未提供profile的内容校验不能证明目标兼容。`review-required`的内部试验包不是已批准发布包。打包目录需独占，不能边修改边校验。

## 按需传输

openvela持有 `/data/models`，Linux使用相对逻辑名，不暴露任意文件路径。OPEN取得size/hash，READ通过受限共享窗口传输，CLOSE释放；超时取消OPEN摘要，避免遗留句柄。临时文件在完整校验成功后才作为可加载模型。摘要旁挂cache是否有效必须绑定文件身份/版本，不能只信同名.sha256。

## 成功案例与口径

9月20日875760324-byte MiniCPM模型经eMMC/openvela/共享窗口到Linux tmpfs，传输段约44MiB/s；首次端到端含两端摘要82.839s，再拉复用 `reused=1 bytes=0 ms=7`。随后RKLLM实际加载成功。不同口径不能合并成“875MB只需7ms传输”。

## 失败反例

此前scp中断留下截断SFace、另一构建树有等大小全零SV6621固件；大小检查不够，必须核对可信hash。Git-Bash路径转换还曾把ADB目的路径改成Windows路径并假成功；读回路径/大小/hash才能证明到位。

验收给定“模型格式正确但runtime版本不匹配”应拒绝兼容；断点续传换了文件hash不得接着旧offset；Linux剩余RAM不足时不能用无限重试替代容量规划。
