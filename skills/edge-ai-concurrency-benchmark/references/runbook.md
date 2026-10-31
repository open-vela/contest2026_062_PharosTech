# 矩阵与R038/R039案例

## 逐请求字段

`run_id、boot_id、model_hash、input_id、warm/cold、resident_set、concurrent_set、start/end、first_callback、ret、output_count/hash、cpu_time、rss/pss、MemAvailable、temperature、frequency`。
音频任务另记真实audio_seconds，RTF=inference_seconds/audio_seconds；有NaN或失败时不算有效音频时长。LLM记录prompt/completion tokens与KV状态。

## 最小矩阵

保持三模型驻留，分别运行A、B、C、AB、AC、BC、ABC；若问题只涉及两模型则不强求三路。启动并不保证计算重叠，记录各进程RUN及实际计算区间。多次样本报告中位数/范围及失败数，样本不足不要算“长期P99”。

## 已有案例

K7 R038三路驻留：单ASR约0.774s，单TTS1.631s，单LLM2.540s；三路同算分别1.445/2.147/4.672s，LLM首回调2.278s。LLM+TTS主要增加首回调前延迟，调度争用是推断，未靠内核trace定因。

稳态PSS合计约708MiB，系统MemAvailable减少约1.314GiB，两者不是同一个数。R039加入初始化采样，最低MemAvailable约2.007GiB，显著低于只看稳态的结果。

## 反例

R038热缓存同时重载约8秒ready，不能与冷顺序约27秒直接证明并行加速；后续R039冷同时加载才提供可比较观察，且各一次不能当严格统计加速比。

模型常驻、无swap、不同runtime共享NPU时，CPU进程RSS不包含全部DMA/NPU占用。NPU中断增量可辅助证明执行，不能换算利用率。没有外部功耗仪，不填瓦数/焦耳。

## 输出决策

先给同驻是否可行，再给初始化峰值、交互延迟及并发代价；推荐策略只能覆盖当前输入和版本。文件ASR快于实时，不等于麦克风流式首字延迟合格；固定音素TTS不等于完整文本前端性能。
