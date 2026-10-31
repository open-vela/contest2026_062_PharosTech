---
name: rknn-model-conversion-validation
license: Apache-2.0
description: "把ONNX模型或可拆分子图转换到RKNN，验证预处理、布局、动态长度、量化/FP16和CPU/NPU数值一致性。用于指定模型的正确转换，不把转换成功当完整产品效果通过。"
---

# RKNN 转换与数值验证

先保留原模型和CPU oracle，确定要证明的子图与输入输出合同，再选优化方式。

- 审原图中的normalize、颜色/布局转换与shape；外部重复归一化会使可运行模型输出错误。
- 拆图、Conv维度提升或固定bucket等变换先在CPU对同输入验证，误差阈值在看目标结果前定义并说明依据。
- 确认SDK实际接受的输入layout/dtype与转换路径，检查日志和返回数据；返回0不保证没有内部转换警告。
- 动态序列补零不自动保持卷积边界；必要mask随每级时间尺度传播，检查有效区末尾和无效区，不只比较前一段。
- 板上同时核对真实NPU执行、全输出有限值、数值误差、有效长度与含转换/拷贝的端到端时间。

[Melo与SFace反例](references/runbook.md)、[来源](references/sources.md)。输出模型hash、转换命令/版本及明确适用输入范围；完整任务准确率/音质另验。
