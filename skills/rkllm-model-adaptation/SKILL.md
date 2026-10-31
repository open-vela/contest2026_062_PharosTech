---
name: rkllm-model-adaptation
license: Apache-2.0
description: "定位 RKLLM 模型加载、乱码、chat template、tokenizer、工具输出及性能问题，建立与原模型参考实现一致的token输入链。用于指定模型适配，不假定所有模型共用同一模板。"
---

# RKLLM 模型兼容与文本前端

先冻结原模型、转换后模型、tokenizer/template、转换器、runtime/驱动和目标SoC，再讨论性能。

1. CPU参考先取得同一messages/tools的原始prompt、token IDs与decode结果；区分模板、分词和推理错误，不只比较最终自然语言。
2. RKLLM内置prompt路径不兼容时，使用该runtime支持的token-input路径并关闭重复模板/BOS；先短句和已知token，再接完整对话。
3. 迁到无Python目标机时做逐token golden和边界fuzz；覆盖added/special tokens、Unicode、JSON数字/字符串、空消息和流式UTF-8切分。
4. 工具解析验证合法结构、参数和finish原因，不把正文里的“我要调用”当实际调用。原生模型能力与系统工具执行分开。
5. 正确性通过后分别测load、prefill、首回调、decode与缓存。上下文预算包括工具表、历史、persona和max_new_tokens。

[MiniCPM5案例与对照方法](references/runbook.md)、[源码/证据](references/sources.md)。交付固定输入的对齐结果与测量，不承诺换模型无需重新验证。
