# Skill 包验证记录

这里只记录 Skill 和附带脚本的验证，不作为板上运行或评分保证。

## [2026-09-16] 移植 Skill 修订版结构与证据解析回归
- 场景: Windows Python 3.12；团队基线 0a1b7ddf 的独立 Skill 工作区。
- 操作: 在 Skill 目录执行 `python -B -X utf8 scripts/test_validate_evidence_log.py`；使用本机 skill-creator 的 `quick_validate.py` 检查包根目录。根据源码核对 runbook 中 build_sd.sh 的四参数接口。
- 结果: 原文 `Ran 14 tests in 0.095s`、`OK`；结构检查原文 `Skill is valid!`。覆盖围栏代码、否定标签、重复字段、日历日期、章节边界、BOM/全角冒号、历史标签及 CLI strict/JSON/只读行为。
- 结论: 结构与脚本回归通过；未重新执行固件完整构建、刷机、独立模型前向测试，也未获得组委会对 Skill 的验收。历史日志审计还检出需人工解释的旧格式，不能将语法失败等同历史实验无效。
- 置信: 编译通过（宿主脚本与校验实际执行；非板测）

## [2026-09-20] v3 源码定位与故障判别补齐并修复两个日志解析边界
- 场景: Windows Python 3.12；已有Skill v2本地源码。仅文档/宿主脚本验证，不构建或刷写固件。
- 操作: 增加source-map与triage，区分各K7配置和产品准备项；补充构建身份、DMA可达性及cache所有权。给日志解析器先加4项边界测试，再修closing fence判断和异常日期漏计。在Skill目录执行 `python -B -X utf8 scripts/test_validate_evidence_log.py`，运行环境提供的skill-creator `quick_validate.py`。
- 结果: 修复前原文 `Ran 18 tests in 0.095s`、`FAILED (failures=2)`；两项分别为 `test_fence_with_info_does_not_close_code` 与 `test_non_padded_date_is_not_silently_dropped`。修复后18项全部 `OK`；结构检查 `Skill is valid!`。原始失败/通过输出保存于项目 `output/skill-review-20260920/validator-before.txt` 和 `validator-after.txt`，此处是结果摘要。
- 结论: 两个具体解析缺陷已用回归复现和修复。新增运行手册按固定源码/既有CI证据校核，未重新执行目标固件构建、板测或独立模型前向演练；14项旧测试与4项新增边界均运行。语法校验不认证历史实验真假。
- 置信: 编译通过（宿主脚本与结构检查；非板测）
