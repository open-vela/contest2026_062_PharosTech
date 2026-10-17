# AMP FIT 与 N-Boot 边界

`build_amp_fit.sh` 从全新 ITS 生成 external-data FIT，同时装入 Linux Image、K7
DTB、initramfs 和 openvela 四A53固件。它不修改已有 vendor FIT，避免历史上因
`fdtput` 重排 external data 导致其他 payload 被截断或 hash 失效。

固定加载地址：

| payload | load/entry |
|---|---:|
| Linux Image | `0x42000000` |
| K7 DTB | `0x4f000000` |
| initramfs | `0x50000000` |
| openvela | `0x4a400000` |

打包前还会检查最终 DTB 的摄像头所有权；旧 Linux 相机 DTB 必须先经 `linux/camera_ownership.py` 生成独立的计算域版本。

脚本要求提供该固件的展开.config，检查四核SMP、私有堆范围和最终DTB仅有四个
A72 CPU节点，拒绝旧CPU3拓扑。随后验证Linux Image/DTB magic、gzip、openvela 16 MiB
上限及FIT load/entry，并生成各段SHA-256。该配置文件须与输入二进制来自同一次
构建；当前脚本不能证明两者的绑定，也不能替代签名或启动验证。

新FIT声明Linux主核`cpu=0x100`、openvela主核`cpu=0`。旧N-Boot要求`cpu=3`，
不兼容并会拒绝新FIT，不能改回3绕过校验。2026-09-10已使用配套新bootamp完成
4+4与真实health/info板测；这是RAM启动候选，不是AMP槽自动启动发行版。

```sh
tools/amp/nboot/build_amp_fit.sh \
  out/linux/arch/arm64/boot/Image \
  out/linux/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-nyabula-amp.dtb \
  out/nyabula-amp-initramfs.cpio.gz \
  out/openvela/nuttx.bin \
  out/openvela/.config \
  out/nyabula-amp.itb
```

本次用fastboot stage仅下载到RAM，再显式运行bootamp。AMP A/B槽位加载留待
后续接入既有bootctrl，不重写分区布局或复制状态机。
