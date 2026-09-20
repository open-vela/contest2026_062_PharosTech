# 模型上板与按需交付（设计备忘，2026-09-20）

状态：设计。除标注"实测"者外均为读码/读文档结论。

## 约束（已核实）

- product 形态下 eMMC 归 NuttX（/data FAT，约 28 GiB），SD 槽被眼睛模组占用；AMP
  Linux 是 initramfs-only，内核 `CONFIG_BLOCK`、`FUSE_FS`、`USERFAULTFD`、`9P` 均未开，
  `TMPFS`/`SHMEM`/`MEMFD_CREATE`/`RPMSG_CHAR` 已开。内核源码 `/root/openvela/linux-rk3576-amp`
  与工具链都在构建机上，开 FUSE 只需改 `tools/amp/linux/nyabula_amp.fragment` 一行后重编。
- `rkllm_init` 只接受 `model_path`，没有内存缓冲入口；但 `librkllmrt.so` 导入了
  `mmap/pread/posix_madvise`，模型是 mmap 惰性换页的（旧日志：875 MB 模型加载中途
  `VmRSS=252 MiB`）。所以必须给它一个 Linux 名字空间里的"文件"。
- 共享内存：0x47c00000 起 4 MiB，双向零误差（实测）；布局见草稿分支
  `chips/rk3576/include/rk3576_shmem_layout.h`，前 4 KiB 是 arena 头不可覆盖，
  `NYAMP_SLOT_SHARED` 在 0x1000、1 MiB，与 ASR 输入/TTS 输出共用，传模型时须独占。
  租约（lease）由计算域（Linux）铸造，控制域原样回传——保持这个方向。
- 吞吐（实测）：NuttX eMMC 读约 75 MiB/s；WiFi 入站约 2.1 MB/s（875 MB ≈ 7 分钟，
  上传必须可续传）；rpmsg 往返与 shmem memcpy **没有实测数**，定窗口大小前先量。
- 内存（实测）：Linux 可用约 3.65 GiB；LLM 进程峰值约 830 MiB。tmpfs 复制再占
  835 MiB，放得下但翻倍。
- 模型规模：LLM 875,760,324 B；ASR zipformer-zh-14M 4 个文件约 24 MiB；TTS 约 150 MiB。

## 方案

A. 面板上传（与 AMP 无关，可先落地）：仿 `/ota/upload` 做 `/models/upload`，目标
   `/data/models/<kind>/<name>`，去掉镜像魔数检查，上限取 /data 余量，支持
   `Content-Range` + `.part` 断点续传，逐文件 sha256；主题 `models.list/delete/status`。
B. 交付（分两级）：
   1. 先做"拉到 tmpfs"：新增 `NYAMP_SERVICE_BLOB = 9`，NuttX 是 /data/models 下命名
      blob 的服务端，Linux 是按范围拉取的客户端。
      `OPEN(name) -> id,size,sha256`、`READ(id, offset, NYBS 窗口) -> bytes`、`CLOSE`、
      `LIST`。Linux 授予 1 MiB 窗口，NuttX `pread` 填入，Linux 写 `/tmp/models/...`，
      校验 sha256 后把路径交给后端。头/描述符/租约/代际/状态码全部复用现有协议。
   2. 内存吃紧再上 FUSE：同一协议，消费者换成 FUSE 守护进程，RKLLM 的 mmap 缺页
      即按需拉取，去掉 tmpfs 那一份。需先量 rpmsg 往返时延，并用大 `max_read` + 预读。
- 草稿分支 `tmp/amp-compute-draft-20260914` 里的 shmem 驱动、协议、nyampd LLM 服务、
  nyampctl llm 尚未并入 product 分支，是 B 的前置。

## 顺序

1. 量一次 rpmsg 往返与 1 MiB shmem memcpy。
2. 确认 /data/models 下长文件名可用（FAT LFN 已开）。
3. 落地 A。
4. 并入草稿分支的 shmem/协议/LLM 服务。
5. 协议加 BLOB 服务 + 编解码单测（主机侧可测）。
6. NuttX blob 服务端；Linux 客户端 + tmpfs 物化 + 校验；`rkllm_init` 指向 tmpfs 路径。
7. 上板端到端，记录传输秒数与 MemAvailable 峰值；ASR/TTS 同法。
8. 视实测再评估 FUSE。
