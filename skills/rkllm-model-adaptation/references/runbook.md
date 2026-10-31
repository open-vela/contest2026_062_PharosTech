# MiniCPM5案例与对照方法

## 为什么需要自有文本前端

项目MiniCPM5-1B W4A16在RKLLM内置prompt路径曾输出乱码，`rkllm_set_function_tools`也不接受其工具格式。原始chat template、原始tokenizer生成IDs后走 `RKLLM_INPUT_TOKEN` 才得到正确中文；这不是所有RKLLM模型都需要同样绕法。

`tools/amp/chat`为纯C++17，包含byte-level BPE、模板、JSON/Unicode及工具输出解析；不把它称SentencePiece。数字Split和后续正则串联、added token优先级、BOS与特殊token防注入都影响IDs；不能靠“看起来同一句话”证明一致。

## 最小对照输入

- 中英混排、数字分组、连续空白、emoji和非法UTF-8边界。
- 首条/非首条system，多轮assistant/tool响应，空列表与超长上下文。
- arguments为JSON字符串或对象；含换行/引号/XML敏感字符。
- streaming恰好切开多字节字符；拼接结果应等于一次性decode。

有意偏离参考的兼容扩展单列，例如text parts展开、字符串arguments解析；不要把扩展算“与原库所有行为100%一样”。项目记录293 golden与30293 fuzz一致，证明这些固定版本用例。

## 板上成功与限制

9月20日短答约26.1tok/s，天气单工具例24.8tok/s，prefill约3.8ms/token；产品长上下文另例21.4tok/s。不能把不同prompt的峰值当统一速度。950MHz等频率策略取决于当前板/散热，不作为任意设备固定值。

模型面对完整多工具表常不真正调用，是语义能力问题而非tokenizer故障。保持正确文本前端，把可靠动作策略交给独立small-model-device-agent任务，不用拼更多提示词掩盖低命中率。

## 性能验收

相同模型/hash、prompt IDs、生成策略与输出长度，分冷/热文件缓存、KV是否清除、固定前缀是否复用；记录全wall-clock与SDK局部时间。不要把暖加载时间当冷启动，不把首次回调等同第一段可播放语音。
