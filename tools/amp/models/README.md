# Linux 模型接口 v1

本目录实现Linux进程内的typed接口与真实后端适配，不是新增Agent或网络服务。
Core产品编排不在这里；现有nyampd仍是RPMsg入口，本轮不扩充wire service ID或大数据共享协议。
因此本目录不能被表述为“Core已经能跨OS直接调用全部模型”。

## 已实现边界

| 层 | 内容 | 本轮验证 |
|---|---|---|
| `nyamp_models` | 请求/事件类型、常驻句柄生命周期、请求关联、取消、deadline、输出归属 | 主机回放与并发取消测试 |
| `nyamp_sherpa` | 真实sherpa recognizer，分块送PCM、partial/final、逐请求独立stream | x86_64真实公开WAV推理，取消后同recognizer再次识别一致；AArch64编译链接 |
| `nyamp_rkllm` | 真实RKLLM1.3 token输入与增量callback，独立runtime | AArch64编译链接；新适配层未板测 |
| `nyamp_melo` | 真实ORT prefix + RKNN masked512 vocoder，输出owned PCM | AArch64编译链接；新适配层未板测 |

回放backend仅定义在测试翻译单元，不能通过生产配置选中；测试输出明确`no_inference=1`。
真实ASR文件测试则打印`real_inference=1 mic_capture=0`，两者不能混淆。

## 接口契约

公共头 `nyamp_models.h`，工厂 `nyamp_backends.h`，命名空间 `nyamp::models`。
`kInterfaceVersion=1`是此C++ API版本，不是新增线上ABI。
调用方链接对应backend target，只在其进程装入对应vendor库；LLM与TTS文件入口是不同可执行文件。

```cpp
nyamp::models::Session session(
    nyamp::models::CreateSherpaBackend(), generation, monotonic_clock_ms);
auto status = session.Load(verified_model_directory);
// Check status before Run. Populate input with normalized 16 kHz mono samples.
status = session.Run({request_id, generation, deadline_ms}, input,
    [](const nyamp::models::Event &event) {
      // event.output is immutable shared ownership; it may be retained.
      // Return false to stop rather than silently drop data.
      return true;
    });
```

- Load成功后重复Run，不重新加载权重；每个Session绑定一个backend。
- LLM输入完整token IDs，输入+最大输出不超过已测2048 context。未提供任意文本tokenizer。
- ASR输入归一化float32、mono16000Hz，单请求最多60秒。当前调用接收一个完整音频段，
  backend内部按320采样分块并产生partial；不是跨OS实时push接口。
  实时接口是`nyamp_streaming.h`的`AsrStreamBackend/AsrStream`（边收边解码、随时取当前假设、上报endpoint标志），
  `nyampd_asr`用的就是它；整段backend现在建在同一个stream之上，两条路径走相同的解码调用，文件测试输出逐字节不变。
- TTS输入音素IDs+同长度tones、speaker、speed；不是通用文本G2P服务。
  prefix latent超512帧返回unsupported，不截断冒充完整音频。
- Event包含原请求context、递增sequence；数据output为不可变shared_ptr，跨回调保留不会引用SDK已释放buffer。
- accepted Run最终尝试发送一次terminal事件；不合法/未就绪/busy/过期generation/重复ID在接受前直接返回错误，不发事件。
- 同一Session的request_id须递增；Unload不会复用旧ID。generation由所有者提供，创建新服务实例时更新，不能固定为线上示例值1。
- Load/Run/Unload由单一工作线程拥有；Run同步阻塞。Cancel可从另一线程或事件回调调用。
  terminal开始交付后不再接受取消；取消后等待Run返回才能销毁Session、卸载SDK或释放输入。
- sink返回false或抛异常时停止后续数据事件；仍尝试terminal通知，不承诺抛异常的消费者实际收到了通知。
- Clock须单调、不阻塞、不抛异常；线上接入使用既有共享counter口径。deadline=0为不限时。
- Backend不得在Run返回后继续callback，不得并发调用Emit；Unload不得抛异常。
- Backend模型目录必须来自经过清单校验的不可变版本目录。软件库不自行下载、升级或校验第三方许可。

取消是合作式停止：ASR在decode块间检查；Melo在ORT/NPU阶段边界检查，不能抢占已提交的NPU任务；
RKLLM依据1.3头文件的callback返回1暂停，再在run返回后调用abort。这条RKLLM停止路径仍需板测，
不承诺硬实时取消或driver已验证的回收延迟。

## 构建与测试

无vendor依赖的主机回放：

```sh
cmake -S tools/amp/models -B out/models
cmake --build out/models -j4
ctest --test-dir out/models --output-on-failure
```

显式设置以下CMake变量可增加真实backend（不下载或复制第三方代码）：

| target | 变量 |
|---|---|
| nyamp_sherpa | NYAMP_SHERPA_INCLUDE（含c-api.h）、NYAMP_SHERPA_LIBRARY |
| nyamp_rkllm | NYAMP_RKLLM_ROOT（含rkllm.h和librkllmrt.so） |
| nyamp_melo | NYAMP_ORT_INCLUDE、NYAMP_ORT_LIBRARY、NYAMP_RKNN_INCLUDE、NYAMP_RKNN_LIBRARY |

所有library路径必须与目标架构一致。交叉构建设置CMAKE_SYSTEM_NAME=Linux、
CMAKE_SYSTEM_PROCESSOR=aarch64、CMAKE_CXX_COMPILER=aarch64-linux-gnu-g++。

模型目录文件名与资源清单一致：LLM `model.rkllm`；ASR `encoder.onnx/decoder.onnx/joiner.onnx/tokens.txt`；
TTS `prefix.onnx/vocoder.rknn`。ASR文件测试可使用同内容的独立测试目录，不能改原模型权重。

```sh
out/models/nyamp_asr_file_test /path/to/asr /path/to/public-16k.wav
# Following two commands require RK3576 NPU, not a desktop replay backend.
out/models/nyamp_llm_file /path/to/llm /path/to/official-token-ids.txt
out/models/nyamp_tts_file /path/to/tts /path/to/phone-ids.txt /path/to/tone-ids.txt
```

文件入口读取空白分隔十进制IDs；不把BIN fixture当文本读取。TTS入口报告PCM元数据，
不自动播放或写个人音频。LLM当前逐请求清KV，前缀cache优化不在这个首批接口中暴露。

## 回放验收

测试覆盖：重复请求拒绝、generation不符、模型未加载、busy、输入格式、输出类型、
一次terminal、owned输出跨Unload保留、sink背压、过期deadline、取消后重用、
backend错误/异常恢复、跨线程取消。无需模型权重即可运行，不作为质量/性能证明。

真实ASR文件测试：同一recognizer完成一段公开录音→第二请求首partial后取消→第三请求重跑，
要求两次完整文本一致、每次恰好一个terminal、取消请求无final文本。
本轮样例完整请求各10次partial、1次final；取消请求1次partial、0次final；全部符合要求。

剩余：动态LLM tokenizer、通用TTS G2P、跨OS共享音视频buffer/业务RPC、
NPU真实取消与多路并发新接口板测。KWS、认主、rPPG仍在后续批次，不因本轮测试更新为完成。
