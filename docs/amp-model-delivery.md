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

## A 的实现（2026-09-19，交叉 gcc 语法检查通过，未上板）

设备侧 `app/nyabula_core/ny_web_models.c`（Kconfig `NYABULA_CORE_MODELS`，默认开）；面板侧
「设置 → 模型」（`views/device/sections/ModelsSection.vue`，队列在 `stores/modelUploads.ts`，
协议在 `lib/modelUpload.ts`，期望文件名表在 `lib/deviceModels.ts`）。

- `PUT /models/upload?path=<kind>/<相对名>`：`Authorization: Bearer`（规则同 OTA）、
  `Content-Length`（≤ 8 MiB）、`Content-Range: bytes s-e/total`、`X-Nya-Sha256`（整个文件）。
  数据追加到 `<path>.part`，`<path>.part.json` 记 `{total,sha256,received}`；分片必须从
  `received` 开始，否则 `409 {"error":"EOFFSET","received":N}`。断掉的分片已落盘部分保留。
  收齐后整文件回读哈希，一致才改名到最终位置并写 `<path>.sha256`；不一致 `422 EDIGEST` 并删除 .part。
  `Content-Range: bytes */total` + 空体 = 只做收尾（最后一片的应答丢了时用）。
- `GET /models/upload?path=...` → `{received,total,sha256,exists,bytes,fileSha256}`。
- 拒绝：`413 ETOOLARGE`（`reason` = `fat32-file-limit` / `offset-width`）、`413 ENOSPACE`
  （余量 < 需要 + 64 MiB）、`413 ECHUNK`、`409 EBUSY`（同时只收一个）、`408 ETIMEDOUT`（60 s 无数据）。
- 主题：`models.list`、`models.status`、`models.delete {path}`、`models.verify {path}`。
- 当前 product 配置 `CONFIG_FS_LARGEFILE` 未开，`off_t` 为 32 位，单文件上限实为 2 GiB − 1
  （设备在 `models.list.fileLimit` 里如实上报）；要放 2–4 GiB 的文件需打开它。

## 顺序

1. 量一次 rpmsg 往返与 1 MiB shmem memcpy。
2. 确认 /data/models 下长文件名可用（FAT LFN 已开）。
3. 落地 A。
4. 并入草稿分支的 shmem/协议/LLM 服务。
5. 协议加 BLOB 服务 + 编解码单测（主机侧可测）。
6. NuttX blob 服务端；Linux 客户端 + tmpfs 物化 + 校验；`rkllm_init` 指向 tmpfs 路径。
7. 上板端到端，记录传输秒数与 MemAvailable 峰值；ASR/TTS 同法。
8. 视实测再评估 FUSE。

## B.1 实现状态（2026-09-20）

置信：**编译通过 + 主机单测通过，未上板**。顺序里的 5、6 已落地；1（实测往返/吞吐）有了
工具但数还没量；7 待上板。

| 部分 | 文件 | 验证 |
|---|---|---|
| 协议 | `tools/amp/protocol/nyamp_protocol.{h,c}`（service 9、编解码、名字规则、bench 图样） | `nyamp_protocol_test` 主机通过 |
| Linux 客户端 | `tools/amp/nyampd/nyampd_blob.*`（BlobClient）、`nyampd_provision.*`（ModelProvisioner、BlobService）、`nyampd_sha256.*`、`nyampd_frame.*`；`nyampd_llm.*` 的延后 LOAD；`nyampd_main.cpp` 的 Outbox(eventfd)/SharedWindow | `nyampd_blob_test`（进程内 openvela 应答器）主机通过；aarch64 静态链接通过 |
| openvela 服务端 | `app/nyabula_core/ny_compute.{c,h}`（`CONFIG_NYABULA_CORE_COMPUTE`），话题 `compute.status/start/stop` | aarch64-none-elf 真实头文件 `-Wall -Wextra -Wshadow -Wundef` 无告警；**未链接、未运行** |
| 诊断 | `app/nyampctl`：`blob bench`、`blob pull`、`status`；I/O 经 `nyampctl_io.h`，Core 服务运行时走 port | PTY 集成测试 + 真实 nyampd 通过（独立模式）；port 模式未运行 |
| Linux 驱动 | `tools/amp/linux/nyamp_shmem.c`：用户态 mmap 改为 `pgprot_writecombine`（非缓存） | 仅读码；需重编内核 |

方向/ID/generation 的决定写在 `tools/amp/protocol/README.md`「请求方向」一节。

### 为什么动了 Linux 驱动的 mmap（仅推断，需上板确认）

驱动头注释说映射是非缓存的，但只有内核 `ioremap` 是；`mmap` 沿用 `vma->vm_page_prot`，
用户态拿到的是**可缓存**映射。openvela 在另一个 CPU 簇上以非缓存方式写这些页，写事务
不会去失效 A72 簇的缓存。之前「双向零误差」测的是写一次、读一次（首次读必然 miss）；
模型交付是同一地址的窗口连读 835 次，可缓存映射会读到上一窗的旧数据。SHA-256 会把它拦成
`digest-mismatch` 而不是静默损坏，`nyampctl blob bench` 每次填窗换 seed 也正是为暴露它
（`pattern_errors != 0`）。若旧内核上 bench 报错而新内核不报，此推断即升级为实测。

### 上板步骤

前置：固件开 `CONFIG_NYABULA_CORE_COMPUTE=y`（依赖 PRODUCT、RPMSG_CHAR、RK3576_SHMEM），
AMP 镜像带新 `nyampd`（以及重编过的内核）；`/data/models/llm/model.rkllm` 已存在
（面板上传会同时留下 `model.rkllm.sha256`；adb/手工拷入的没有，首次 OPEN 会现算约 12 s 并回写）。

```text
nsh> nyampctl status
  期望 running=1 linked=1 generation=<非0> capabilities=0x00000007（bit2=blob）
nsh> nyampctl health          # 经 Core 服务的 port，不再与服务抢帧
nsh> nyampctl blob bench 200
  记录 rtt_us min/avg/max、fill/copy KiB/s；pattern_errors 必须为 0
nsh> nyampctl blob bench 200 262144     # 换窗口大小对比 fill KiB/s，定窗口
nsh> nyampctl blob pull llm/model.rkllm
  期望逐行进度，末行 files=1 reused=0 bytes=875760324 ms=...；期间另开会话看
  nyampctl status 的 blob offset/rate，或面板 compute.status
nsh> nyampctl blob pull llm/model.rkllm  # 第二次：reused=1，毫秒级
nsh> nyampctl blob pull asr              # 目录：LIST 后逐个拉
nsh> nyampctl llm load llm/model.rkllm   # 逻辑名；绝对路径行为不变
nsh> nyampctl info                       # model=/tmp/models/llm/model.rkllm last_load=0
```

要记进实测日志的数：rtt、各窗口大小的 fill/copy、875 MB 端到端秒数、首次 OPEN 的摘要秒数、
拉取期间 Linux `MemAvailable` 最低值（tmpfs 多占一份 835 MiB）。

失败判读：`status=-2`(NOT_READY) 名字不在 `/data/models` 下；`-4`(STALE_GENERATION) nyampd
刚重启、下一次即恢复；`-8`(BACKEND_ERROR) 多为摘要/大小不符或 tmpfs 写失败，看 Linux 侧
pstore/下次 `nyampctl info`；`pattern_errors!=0` 见上一节。若 `pread` 直写共享区在 eMMC
驱动上有问题（DMA 不接受该地址），开 `CONFIG_NYABULA_CORE_COMPUTE_BOUNCE` 退回中转拷贝。

### 已知限制

- 目录拉取不删除 tmpfs 里上游已不存在的旧文件。
- 对已加载的会话再 LOAD 逻辑名，会先拉完模型再被 Session 以 busy 拒绝（沿用原语义）。
- `linked` 在 nyampd 同步加载绝对路径模型、主循环阻塞期间可能短暂为 false（20 s 超时）。
- 每个端点同时只服务一个需要现算摘要的 OPEN；blob 槽 4 个、port 4 个。
- FAT 无符号链接；`lstat` 逐级拒绝符号链接的分支在板上走不到，属防御性代码。
