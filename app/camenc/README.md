# camenc — 推流服务器参考实现

## 1. 怎么跑

```sh
./build.sh contest2026_062_PharosTech/configs/camera --cmake
# 固件：cmake_out/contest2026_062_PharosTech_camera/nuttx.bin
```

板上：

```
nsh> camenc -p 8080 -n 300
```

然后浏览器访问 `http://<板子地址>:8080/`。

命令行选项：

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `-d` / `-e` | 采集设备 / 编码设备 | `/dev/video0` / `/dev/video1` |
| `-o` | 同时写一份 fMP4 到文件（不给就只编码） | 无 |
| `-p` | WebSocket 端口，`0` 表示不起服务 | 8080 |
| `-n` | 编码多少帧后退出 | 300 |
| `-m` | 传感器模式下标，见下表 | 3 |
| `-q` | 量化参数 0..51 | 26 |
| `-G` | 每组图像数 1..1000 | 15 |
| `-x` / `-g` | 手动曝光（行）/ 模拟增益（16 = 1.0x） | 传感器默认 |
| `-A` | 自动曝光/增益/白平衡 0 或 1 | 1 |
| `-t` | 自动曝光的目标亮度 0..255 | 60 (`CAMENC_3A_TARGET_DEFAULT`) |

★ **`-n` 是帧数不是时长。** `-n 300` 大约是十秒，之后程序退出、端口关闭；浏览器
报"连接超时"时先看是不是程序已经退出了，而不是网络问题。

传感器模式下标（以 `drivers/include/ov5647.h` 为准，**不要凭记忆写**）：

| 下标 | 模式 | 能否零拷贝 |
| --- | --- | --- |
| 0 | 1920x1080@15 | 否（编码器把亮度平面补到 1088 行） |
| 1 | 1296x960@30 | 是 |
| 2 | 1296x960@15 | 是 |
| 3 | 1280x720@30 | 是（默认） |
| 4 | 1280x720@15 | 是 |
| 5 | 640x480@60 | 是 |
| 6 | 640x480@30 | 是 |

## 2. 数据流

```
VICAP（内核 LPWORK 线程）
  │  去马赛克 → NV12 → 清 cache
  ▼
/dev/video0 ──DQBUF──► camenc 主循环
                          │  零拷贝：直接把相机 buffer 交给编码器
                          │  否则：memcpy 进编码器的 buffer
                          ▼
                    /dev/video1（VEPU510 硬件编码器）
                          │  annex-B 访问单元（一个 IDR 或一个 P）
                          ▼
                  camenc_stream_write()
                          │  INIT 段（一次）+ FRAGMENT 段（每帧一个）
              ┌───────────┴────────────┐
              ▼                        ▼
          写文件                  camenc_ws_publish()
                                       │  拷进每个客户端的发送缓冲
                                       ▼
                                   socket → 浏览器 MSE 播放
```

## 3. 代码结构与职责

| 文件 | 职责 | 移植时怎么办 |
| --- | --- | --- |
| `camenc_main.c` | 示例应用：打开两个设备、协商格式、主循环状态机、CLI、3A 接线、诊断报告 | **照骨架改**：循环怎么组织、编码器/相机缓冲怎么排队、零拷贝怎么判定 |
| `camenc_stream.c` / `.h` | annex-B → fMP4：换 framing、抽参数集、按访问单元切段 | **整文件复用**。纯 C、只依赖 libc、没有 socket / 文件 / 配置，可以在主机上编译 |
| `camenc_ws.c` / `.h` | 单端口 HTTP + WebSocket 服务器，内嵌播放页，非阻塞、无自己的线程 | **整文件复用**，或只借用它的推送语义（见第 5 节路线 B） |
| `camenc_3a.c` / `.h` | 曝光、增益、白平衡的**决策**部分（测量进、设置出，不碰设备） | 可选复用；接口是纯函数，主机上可测 |
| `web/page.html` | 浏览器侧全部代码：MSE 播放 + 控制 UI | 改 UI 就改这里 |
| `tools/test_camenc_3a.c` | 3A 的主机侧测试 | 参考 |

### 三个关键接口

移植时只要盯住这三处，其余都是应用逻辑：

```c
/* 1. 封装层的输出：camenc_stream 把段交给谁 */
typedef int (*camenc_emit_fn)(void *arg, enum camenc_seg_e seg,
                              const uint8_t *data, size_t len, bool key);

/* 2. 服务器不等待、不开线程：向调用者的 poll() 要位置 */
int  camenc_ws_fds(struct camenc_ws_s *ws, struct pollfd *fds, int max);
void camenc_ws_ready(struct camenc_ws_s *ws, struct pollfd *fds, int n);

/* 3. 把段推给所有客户端（形状与 camenc_emit_fn 一致，所以接起来就一行） */
int  camenc_ws_publish(struct camenc_ws_s *ws, enum camenc_seg_e seg,
                       const uint8_t *data, size_t len, bool key);
```

`camenc_ws_*` 的形状是刻意的：它不阻塞、不开线程，把 fd 交给调用者一起 `poll()`。
**这是它能塞进任何已有事件循环的原因**，也是移植时最该保留的性质。

### 主循环：一个线程，一个 poll，一帧跨两轮

```c
for (; i < frames; )
  {
    /* 轮 A：把一帧交给编码器就返回，不在这里等 */
    如果 inflight：先收上一帧的码流（DQBUF 编码器 capture）→ mux → 分发 → 归还容器
    取相机帧（DQBUF，非阻塞）→ 交给编码器（QBUF）→ 记下 inflight/camidx/pts

    /* 等到有 fd 就绪，或 500 ms 安全网超时 */
    camenc_wait(camfd, encfd, inflight);   /* 相机 + 编码器 + 服务器所有 fd 一起等 */
  }
```

要点：

* 相机 fd 是 `O_NONBLOCK` 打开的；`EAGAIN` 表示"还没有"，不是错误。
* 编码器 `QBUF` 提交即返回（驱动侧把一次编码拆成 start / finish，完成在高优先级
  工作队列上），所以循环在编码的 ~24 ms 里可以去服务 socket——这正是这套结构存在
  的理由。
* `CAMENC_WAIT_MS`（500 ms）**只是安全网**，不是轮询间隔：相机每秒会唤醒循环 30–60 次。
* `inflight` 以及相机 buffer 下标、时间戳必须活在循环外面，因为它们跨两轮。

## 4. 协议与流格式

**HTTP/WebSocket**（`camenc_ws.c` 里自己实现的子集，只够单向推流用）：

* 任何 `GET` → 返回内嵌页面（`Content-Type: text/html`；页面是唯一资源，
  不区分路径）。
* 带 `Upgrade: websocket` 的请求 → 101 握手（`Sec-WebSocket-Key` → SHA-1 + base64）。
* 其余请求 → 404。
* 只实现：握手、出站 text/binary 帧（服务端不掩码）、入站 close/ping、7/16/64 位
  长度。不实现分片、扩展、子协议，遇到就按协议拒绝。

**fMP4 分段**：

* `INIT` 段一次，含参数集和 track 描述；`FRAGMENT` 段每个访问单元一个。
* 段里的 `key` 来自 `V4L2_BUF_FLAG_KEYFRAME`（编码器填的），表示"这一段可以作为
  流的起点"。
* annex-B 只换 framing（起始码 → 长度前缀），**不是转码**；模拟防止字节保持不变。
* 参数集从第一个访问单元里抽出来放进 `INIT`，所以 **`INIT` 必须等第一个访问单元
  才能生成**——这是段的顺序不能颠倒的原因。
* 晚到的客户端拿到 `INIT` 后必须**等下一个 key 段**，中间那些段直接丢掉（不排队、
  不重放）。等待代价是最多一组图像（这里 15 帧 ≈ 半秒）。

**codec string**：`avc1.PPCCLL` 三个字节从 SPS 里读（`camenc_stream_codec_string()`）。
浏览器的 `MediaSource` 会拒绝 codec string 与实际数据不符的流，所以它不能猜、不能
写死。页面里有一个 **11 字节等长占位符** `@@@@@@@@@@@`，服务器返回页面时原地替换，
这样回复长度不变、也不需要把整页放进一个 buffer。

**控制通道**：客户端发一行 `名字 数字`，不认识的静默忽略（不因此断开一个正在看
流的客户端）：

| 消息 | 含义 |
| --- | --- |
| `ae <0\|1>` | 自动曝光/增益 关/开 |
| `e <lines>` / `g <gain>` | 手动曝光 / 增益（会把自动曝光关掉） |
| `aw <0\|1>` | 自动白平衡 关/开 |
| `kr/kg/kb <gain>` | 手动红/绿/蓝增益（256 = 1.0），会把自动白平衡关掉 |
| `mo <index>` | 切换传感器模式 |

**一个座位**：同一时刻只服务一个推流客户端；新的升级请求会把旧的踢掉（last one
wins）。刷新页面能立刻拿到画面，靠的就是这条规则，而不是等旧连接被回收。

## 5. 移植到 nyabula

本仓当前分支里，唯一的 HTTP/WebSocket **服务器**实现就是 `camenc_ws.c`
（`nyabula_core/ny_http.c` 是出站 HTTP 客户端，不是服务器）。两条路线：

**路线 A（最小改动）**：把 `camenc_stream.c`、`camenc_ws.c`、`web/page.html`
一起搬过去，由你们的事件循环线程带着它（`camenc_ws` 自己不开线程），占一个端口。
`camenc_main.c` 里需要照抄的是：

* 两个设备的打开与格式协商（`VIDIOC_S_FMT` / `REQBUFS` / `QUERYBUF` / `MMAP`）；
* 主循环的两轮结构、`inflight`、`camenc_dequeue()` 的 `EAGAIN` 处理；
* 零拷贝判定（见坑 10）；
* `camenc_ui_command()` 与 3A 的接线（可以照搬或换成 nyabula 自己的控制面）。

**路线 B（nyabula 已有自己的服务器/端口）**：只搬 `camenc_stream.c`。它与服务器
之间只有 `camenc_emit_fn` 一个耦合点，把 `camenc_ws_publish()` 换成你们自己的
推送实现即可。此时要自己补上 `camenc_ws.c` 已经替你们解决的三件事：

1. 晚到客户端的 `INIT` 补发与"等下一个 key 段"；
2. 有界发送缓冲 + 落后的客户端**丢掉而不是背压**；
3. 页面里的 codec 占位符替换。

**必须一并满足的构建配置**（`camenc_ws.c` 用 `#error` 强制了前三个，照搬就不会踩；
路线 B 自己写服务器时没有这层保护——而它们的失败方式都不是编译错误，是"网络莫名
奇妙坏掉"）：

* `CONFIG_NET_TCPBACKLOG` —— 没有它 `accept()` 没有 backlog，`camenc_ws.c` 直接 `#error`；
* `CONFIG_NET_TCP_WRITE_BUFFERS` —— 没有它 `send()` 不看非阻塞标志、会一路阻塞，`#error`；
* `CONFIG_NET_SOLINGER` —— 没有它丢弃客户端只能温柔关闭，连接会带着发送缓冲重传几分钟，`#error`；
* `CONFIG_NET_ALLOC_DEVIF_CALLBACKS=1`、`CONFIG_NET_TCP_ALLOC_CONNS=1` —— 见坑 1。

**内存与线程注意**：`struct camenc_ws_s` 约 8 KiB（四个客户端的接收缓冲），
**不能放在栈上**（应用栈默认 8192）；camenc 自己是单线程事件循环，如果 nyabula
用多线程，`camenc_ws_*` 的所有调用必须固定在同一个线程里。起始时要给
`camenc_ws_start()` 一个发送缓冲大小 `tx_size`，它**至少要装得下一个段加一个帧头**，
否则任何客户端都永远发不出去（camenc 用的是 `CAMENC_WS_MAX_SEGMENT`，512 KiB）。

## 6. 踩过的坑

### A. 网络栈与板级配置（最贵的两个，各花了几天）

1. **两个网络结构池是硬上限，不是初始值。**

   | 池 | 配置 | 耗尽后 |
   | --- | --- | --- |
   | socket 事件回调 | `CONFIG_NET_ALLOC_DEVIF_CALLBACKS` | `tcp_pollsetup()` 返回 `-EBUSY`，进而 `poll()` 不再等待（见坑 2） |
   | TCP 连接结构 | `CONFIG_NET_TCP_ALLOC_CONNS` | `tcp_input()` **静默丢掉 SYN**，浏览器报连接超时，服务器这边一个错误都没有 |

   两个都要设成 **1**（不是更大：`1` 表示按需分配、用完归还；`2` 以上会成批分配并
   永久保留），并且**不要靠调大 `PREALLOC_` 解决**——`TIME_WAIT` 是几十秒，浏览器
   一次刷页会开好几条连接，任何固定数字都是更远的天花板而已。

2. **`poll_setup()` 的缺陷会吞掉整个数组。** NuttX 的 `fs/vfs/fs_poll.c` 里，只要
   有一个 fd 注册失败，它就给这个 fd 标上 `POLLERR` 并 `return count + 1`，
   **其后的所有 fd 都不再检查**。两个后果叠加：`poll()` 带着一个没发生过的错误
   立刻返回（表现为"poll 不等待"、循环空转），而"等待"恰恰是这些回调唯一被注册
   的时机——于是 `poll()` 永远收不到连接断开的事件，客户端也就永远不被释放，
   池子一直被占着。camenc 曾经因此出现"服务器看起来根本没在 accept"的现象。
   *这是 NuttX 侧的缺陷，值得上报*：正确的做法是标记失败的那个 fd 后继续。
   诊断见第 7 节。

3. **非阻塞 `recv()` 在对方正常关闭后返回 `EAGAIN` 而不是 0。**
   `net/tcp/tcp_recvfrom.c` 里，对方发 FIN 后阻塞路径正确返回 0，非阻塞路径返回
   `-EAGAIN`，而 POSIX 要求返回 0。所以单线程（必须非阻塞）的服务器看不见"对方
   正常关闭"，只能靠：`SO_ERROR` 检查（RST 是可发现的）+ 自己的停滞计数器
   （见坑 8）。同样值得上报。

4. **`CONFIG_NET_TCPBACKLOG` 必须开。** 没有它，连接只有在"正好有任务阻塞在
   `accept()` 里"时才会被交接；编码器正忙时到达的连接会被丢掉，浏览器看到拒绝或
   超时。`camenc_ws.c` 用 `#error` 保证这一点。

5. **`CONFIG_NET_TCP_WRITE_BUFFERS` 必须开。** 关掉时 `send()` 内联等待确认，而且
   **完全不看 `O_NONBLOCK`**（`tcp_send_unbuffered.c` 里没有任何相关判断），
   默认发送超时还是"无限"。表现不是卡死，而是**画面一动帧率就掉**：大帧意味着更多
   往返，往返在取帧的循环里。实测把 62.5 fps 压到 40 以下，而编码器自己只用 1.5 ms。

6. **`CONFIG_NET_SOLINGER` 必须开。** 丢弃客户端要用 abortive close（`SO_LINGER`
   置 0）：否则 `close()` 会等对方确认，连接带着发送缓冲重传几分钟，而这些缓冲正是
   所有连接共享的池。不开的失败方式极其隐蔽——应用数字一切正常，池子在随后几次
   刷新里慢慢被抽干。

7. **关闭分两种，别用错**：正常回完最后一个包用温柔 `close()`（数据还在写缓冲里，
   reset 会把它丢掉）；丢弃客户端用 `SO_LINGER 0` 中止连接。丢弃时数据已经不会被
   确认了，温柔关闭只会拖住缓冲。

8. **不要依赖 `poll()` 一定汇报"对方走了"。** 最终保留的兜底是 `camenc_ws_expire()`：
   一个客户端在有待发数据的情况下连续 5 秒一个字节都没发出去，就当它不读了，
   直接释放座位。这不是 workaround，而是策略。

### B. 设备与驱动

9. **两个设备对"这个 buffer 是谁"的回答方式不同。** 相机看 `v4l2_buffer.index`；
   编码器**完全忽略 index**，只看 `m.offset`（即 `VIDIOC_QUERYBUF` 回报的值）。
   只填 index 的排队请求会让编码器拿到一个从 0 算出来的地址。camenc 的做法是保存
   `QUERYBUF` 得到的描述符并原样排队（ffmpeg 的 v4l2m2m 后端也是这么做的）。

10. **零拷贝的前提是"编码器能访问这个地址"，而不是"这个 buffer 是我分配的"。**
    VEPU 的 MMU 处于透传，寄存器里放的必须是**物理地址**；全芯片只有 DMA heap
    （identity-mapped）满足这一点。所以：

    * 编码器输入侧要接受 `USERPTR`，判定条件是硬件的要求——**两端 64 字节对齐、
      整体低于 4 GiB**——而不是"是不是我分配的"；
    * 相机驱动的帧缓冲要改从 DMA heap 分配（`alloc`/`free` 钩子）；
    * camenc 侧要按帧导入地址（`camenc_setup_import` / `camenc_queue_import`）。

    相机写的是"画面几何"（亮度 stride = width，色度在 `width × height`），而编码器
    按**分配高度**读（色度在 `y_stride × v_stride`，`v_stride = align(h, 16)`）。
    当 `height` 是 16 的倍数时两者一致 → 可以零拷贝；**1920x1080 是唯一的例外**
    （1080 → 1088），必须走拷贝路径。

11. **`src_size`/`dst_size` 是分配尺寸，不是画面尺寸。** 用 `w*h*3/2` 去填 NV12 会
    在高度不是 16 倍数时出错：1080p 下色度平面会早 8 行，屏幕底部出现**一条品红色
    横带**（那 8 行色度从没被写过，读的是内存里的旧数据）。这个 bug 在其它模式下
    完全看不出来，因为那些模式下 `align(h,16) == h`。

12. **缓存要显式维护，而且别越界。** VEPU 的 DMA 不做 snooping：VICAP 在去马赛克后
    clean，VEPU 在编码前 clean 源、编码后 invalidate 目标。DMA heap 是正常的可缓存
    内存、64 字节 cache line，所以 buffer **两端都要 64 字节对齐**，否则维护会碰到
    邻居。

13. **相机 buffer 要尽早归还。** 它在"编码器把输入容器还回来"的那一刻就已经读完，
    不要拖到帧尾的 mux/emit 之后——在零拷贝模式下那个 buffer 正是编码器的源，
    capture 驱动会因此少一个 buffer。改早之后看 VICAP 自己那行报告确认：
    `VICAP: frames N, drops N (overrun 0, no buffer 0, stale 0, queue 0), ...`
    ——四个 drop 计数全为 0 就是没有缺过 buffer。

14. **模式切换不要走 uninit/init。** 必须用 `rk3576_vicap_reconfigure()` /
    `rk3576_csi_host_set_lane_rate()`；uninit/init 那条路只试过一次就把板子挂死并
    触发了看门狗。另外切换只能在"没有帧在编码器里"的时候做，否则会把旧尺寸的帧
    混进新流。

### C. 构建与运行

15. **改了 `defconfig` 不会重新展开 `.config`。** CMake 只在文件缺失或*路径*变化时
    重新生成，增量构建会静默用旧值，表现像"我的代码改动弄坏了它"。改完要
    `rm -rf cmake_out/<cfg>` 重建，并在 `cmake_out/<cfg>/include/nuttx/config.h`
    里确认选项真的生效了。

16. **改 `web/page.html` 不会触发重编。** 页面通过 `.incbin` 汇编指令进固件，
    编译器的依赖扫描看不见它，板子会继续发旧页面，看起来像浏览器缓存。
    `camenc/CMakeLists.txt` 用 `OBJECT_DEPENDS` 补上了这一条；搬走时别丢。
    验证方法：`grep -ao "<新页面里的某个字符串>" cmake_out/<cfg>/nuttx`。

17. **`.incbin` 的路径相对编译器的工作目录。** Make 构建下 `Config.mk` 会
    `make -C system/camenc`，所以 `web/page.html` 能解析；CMake 构建下靠
    `INCLUDE_DIRECTORIES` 指进去。换了构建方式要重新确认。

18. **`struct camenc_ws_s` 不要放栈上**（约 8 KiB > 默认 8 KiB 栈）。camenc 把它放在
    `.bss`。

19. **别忘了 `-n` 会退出**（见第 1 节）。

### D. 流与协议

20. **丢掉一个 fragment 会毁掉它后面所有 P 帧。** 片子丢一帧，解码器拿错参考，
    显示的不是"少一帧"而是一片花。恢复方式是把客户端标成 `waiting`，跳过中间所有
    段，直到下一个 key 段。**不要把中间的段排队等它**——那是"重放"，重放的数据量
    按定义比它等待期间新来的帧还多，一定塞不进有界缓冲（camenc 早期版本就是这么
    在握手期间把客户端挤掉的，浏览器那侧表现为"根本连不上"）。

21. **codec string 必须来自 SPS。** 猜一个"看起来合理"的 `avc1.xxxxxx` 会在别人的
    机器上失败，而且失败在浏览器里，不在板子上。

22. **`INIT` 段必须先发**，而且只有见到第一个访问单元才能生成。晚到客户端 =
    `INIT` + 等 key 段。

23. **一个座位**：新升级请求踢掉旧连接。多观众不是这套设计的目标；要支持多观众，
    发送缓冲和码率都需要重新算（每个观众一份发送缓冲，落后的一样要丢）。

24. **没有码率控制。** 编码器跑固定 QP，帧率只进 VUI（告诉播放器怎么呈现，不决定
    编码器跑多快）。想控码率只有两个旋钮：`-q` 和 `-G`。

25. **帧率与单核预算。** 这是单核系统：在 1296x960 上实测，VICAP 的去马赛克每帧约
    22.9 ms，应用侧的非等待工作约 12 ms，加起来 34.9 ms 对 32 ms 的帧周期，约
    27.4 fps。把帧率拿回来要靠**减少工作量**（零拷贝就是为这个），而不是加线程：
    单核上拆线程不减少总工作量，只减少"结构上的风险"。`copy` 与 `camera` 两个阶段
    的读数就是判断零拷贝有没有真正生效的地方。

## 7. 观测与诊断

程序每 100 帧打一次报告（`CAMENC_REPORT_FRAMES`）。几个要会读的地方：

```
  us/frame: camera N/M copy N/M encode N/M drain N/M 3a N/M status N/M mux N/M
            file N/M clients N/M serve N/M accept N/M read N/M flush N/M
            sum N vs period N
  frames: N IDR, N B mean; N P, N B mean (N% of an IDR)
  poll: N rounds, N timeout, N camera, N encoder, N listen (N in, N err),
        N client (N out, N err), N failed (errno N)
  mem:  iob N free (N for tx), heap N free, N held
CAMENC WS: N client(s), pending N, sent N, skipped N, served N, dropped N
```

* **`camera` 是"等"的时间**，不是取帧的耗时（阶段改名过一次，名字没改，含义看代码）。
  `file`/`clients`/`serve`/`accept`/`read`/`flush` 是前面几个阶段的细分，
  **不要重复计入 sum**。
* **`poll: rounds` 应该约等于这一窗做了多少事**（100 帧 ≈ 100 次相机 + 100 次编码器）。
  上千就是 `poll()` 根本没在等 → 回头看坑 1/2。
* **`listen (N in, M err)` 里 `err` 非 0 而 `in` 为 0** = 监听 fd 注册失败（回调池短了）。
* **`client (N out, ...)` 的 `out` 一直是 0** = 写侧从来没被事件驱动过，说明注册没成功。
* **`accepted` 停在 0、浏览器说连不上、而报告里一个错误都没有** = TCP 连接池短了
  （SYN 被静默丢弃）。去日志里搜 `No free TCP connections`。
* **`mem: iob ... free`** 正常就说明不是 IOB/堆的问题——当初这里一直很健康，因为
  被耗尽的根本不是这两个池。**这个不对称是当时最费时间的地方。**

服务器自己还有一行 `camenc_ws_report()`，`sent`（socket 真的收下了多少）和
`skipped`（没塞进缓冲被丢了多少）是判断"某个观众是不是跟得上"的唯一依据——
观众收不到和观众收得很顺，从取帧循环看是一模一样的。

`-o` 写出来的 fMP4 可以直接用 `ffplay` 播放，用来把"编码问题"和"网络问题"分开：
文件对而浏览器不对，问题在服务器；文件也不对，问题在采集/编码。

## 8. 源码里更长的说明

几处设计理由写在代码注释里，改到那里之前建议先读：

* `camenc_ws.c` 文件头：这个服务器历史上三个"看起来无关"的故障、两个配置根因、
  怎么从报告里认出来、以及两个仍然存在于 NuttX 的缺陷（坑 1–3 的完整版）。
* `camenc_ws.h`：为什么 WebSocket 而不是 HLS、为什么"落后就丢"、为什么客户端要等
  key 段、`CAMENC_WS_*` 各个上界的来历。
* `camenc_stream.h`：annex-B 与 MP4 的两点差别、为什么参数集要出带。
* `camenc_3a.h`：为什么曝光/增益/白平衡是一个循环而不是三个。
* `camenc_main.c` 文件头：两个设备在排队语义上的差别、什么时候拷贝。
