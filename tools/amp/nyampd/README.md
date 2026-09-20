# nyampd

`nyampd` 是 Linux AMP 计算域唯一的控制入口。它直接读写由
`rockchip_rpmsg_mbox` 和 `rpmsg_char` 创建的消息型字符设备；默认从sysfs寻找名字为
`rpmsg-raw`的endpoint对应的`/dev/rpmsgN`，不依赖动态编号。收到畸形包时丢弃，
transport EOF/错误或短写时立即退出，让 PID1 重新拉起并生成新的 generation。

打开端点后先发送READY事件完成地址发现；已实现 health/info、LLM 与模型交付(BLOB)。NPU、ISP、ASR/TTS必须在各自
vendor runtime 和 buffer 生命周期确定后作为独立提交加入，不能先返回伪成功。

模型交付：计算域没有存储，模型在 openvela 的 `/data/models`。`BlobClient`(nyampd_blob.*)
经 `NYAMP_SERVICE_BLOB` 以 1 MiB 共享内存窗口把命名 blob 拉到 tmpfs
（默认 `/tmp/models/<name>`，可用环境变量 `NYAMPD_MODEL_ROOT` 改），增量 SHA-256 与 OPEN
给出的摘要、大小核对后才落成正式文件并写 `<file>.sha256` 标记；标记与大小都吻合则直接复用。
`ModelProvisioner`(nyampd_provision.*) 供 LLM LOAD 使用：相对的逻辑名（文件或目录）先补齐
缺失文件再把 tmpfs 路径交给后端，绝对路径原样直通。拉取在 worker 线程进行，主循环继续收发
（它正是投递 BLOB 应答的那个循环），LOAD 的应答延后经服务队列发出；期间以 LOAD 的
request_id 发 `BLOB/EVENT_PROGRESS`。`BlobService` 处理 openvela 发来的 `BENCH_RUN`
（rpmsg 往返时延 + 共享窗口吞吐，带位置相关图样校验）与 `PULL`（只拉不加载）。
RESPONSE/EVENT 帧永不被应答。单测 `nyampd_blob_test` 用进程内的 openvela 应答器验证。

deadline 使用两端共享的 ARM generic counter 换算毫秒。AArch64 生产构建读取
`cntvct_el0/cntfrq_el0`；主机单测显式注入当前值，不依赖主机时钟。

主机构建与测试：

```sh
cmake -S tools/amp/nyampd -B out/nyampd
cmake --build out/nyampd
ctest --test-dir out/nyampd --output-on-failure
```

运行：

```sh
/usr/sbin/nyampd
```

也可在诊断时显式传入设备路径。openvela侧`nyampctl health`首次运行会创建并公告
`rpmsg-raw` channel，initramfs supervisor会在endpoint出现后自动拉起daemon。
