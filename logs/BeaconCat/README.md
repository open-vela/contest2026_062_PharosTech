# BeaconCat AI Coding 日志

本目录归集Nyabula项目11个会话，共11,848条标准事件。保留各会话ID、seq、时间、角色与已有工具事件；没有删掉失败步骤或补写历史结果。

| 来源 | 会话 | user | assistant | tool | 总事件 |
|---|---:|---:|---:|---:|---:|
| Codex Desktop | 7 | 1694 | 8379 | 0 | 10073 |
| Claude Code | 4 | 157 | 472 | 1146 | 1775 |

`user`角色中包含采集器保留的通知/上下文，不等同人工提问次数；工具调用和返回分别计数。会话之间存在继承，事件总数不等于独立贡献量或Token用量。

## Codex 来源与范围

明确选取：Nyabula 一门双至尊、Nyabula 总研发空间、Nyabula 硬件探索者、Nyabula 内核研究者、Nyabula 赛博坠机者、Nyabula 喵卡贝拉、Nyabula 硬件坠机者。排除Nyabula 最终讲述者，没有按相同工作目录自动扩大采集范围。

解析来自 [open-vela/.claude PR52](https://github.com/open-vela/.claude/pull/52) head `1f4d1b1d6992f0fcd8201e7808a773482e6ac02c`，结合明确会话白名单调用。原始cwd不在.repo工作区，原版CLI会跳过，因此不宣称未经包装的CLI直接导出成功；没有改源cwd、创建伪.repo或伪造时间。

7个任务共有24段本地rollout。先处理全部历史段，再按原时间归并；保留源hash与大小。原版按session_id幂等跳过会遗漏后续同ID段，本批没有只取当前文件指针。跨段完全相同事件的去重数量为0；没有对跨任务的继承内容做删改。

原解析器只生成用户/助手文本，工具与非文本不进入标准事件。本目录不包含本地另存的105,756条Codex工具记录、253条附件条目及usage等补充数据，也没有将它们伪装为已进入本批标准日志。系统/开发者初始化与推理内部记录不进入Codex正文。

manifest保留原工具 `collection_mode=backfill-sqlite` 的历史枚举拼写，并用 `source_format=codex-rollout-jsonl` 说明真实来源。`health=degraded`表示标准文本未覆盖其他记录类型，不表示源文件损坏。格式校验通过不等于赛事已经认可非.repo定向补导方式，来源与方法保留供审阅。

## Claude Code 来源与继承关系

使用2026-09-16由官方collector 1.3.0转换的4份历史开发候选，保留工具调用/返回及原有元数据；不包含两个安装测试会话和仓库示例。修正过manifest路径中的Windows反斜杠，没有因此修改JSONL内容。

| session_id | 内容概括 | 原始事件UTC日期 | 事件数 |
|---|---|---|---:|
| `eb4b3d82-cf5c-4753-b0ca-7c505f992c6f` | 初版双猫眼Web Demo与表情动画 | 2026-08-15 | 125 |
| `7e740137-333b-4fce-ab1e-14965b5f883e` | Core审核修复、Make/CMake与sim验证 | 2026-08-29 | 302 |
| `53149ad7-206f-4cc7-98ea-be54f9b18bbc` | Flutter/Vue、Go Cloud/Simulator、NyaLink/NyaUI | 2026-08-29—08-30 | 438 |
| `d92287fc-5656-4959-bc0b-971e5b3938aa` | 继承Core会话后追加WebUI三端重构 | 2026-08-29—09-09 | 910 |

忽略session_id后，`d92287fc`的前302条与`7e740137`全部302条逐事件相同。两份原始会话均保留并明确说明，不应重复算作独立产出。其他Codex任务也可能继承上下文。

Claude文件目录及旧manifest起止字段中的9月16日是此前导出时刻；真实对话日期以JSONL的`ts`为准。未把这些旧字段重新编造为其他时间。manifest里的模型字段继承源导出，应结合逐事件字段理解。

## 发布脱敏与完整性

原导出仍保留在本地；提交副本通过同一官方 `snapshot_core.redact_value` 及其支持的自定义规则处理遗留VNC/无线访问口令。源码、历史事实、时间、顺序与数量不因脱敏改写；测试类型注解、SSH密钥文件名等误报不作凭据删除。

本批额外进行了59次凭据替换，涉及50个事件。manifest逐会话记录：

- `source_export_sha256`：发布脱敏前JSONL摘要；
- `publication_export_sha256`：本次提交JSONL摘要；
- `publication_redaction`：所用官方实现版本和该会话替换统计；
- Codex既有 `source_integrity`：原rollout的hash、大小与历史段清单。

具体口令及包含口令字面量的本地脱敏配置不入仓。`.gitattributes`禁止Git转换JSONL换行，确保仓库blob与发布摘要一致。原始导出文件未被覆盖。

## 验证

使用PR52固定版本的官方 `tools/validate-log.py` 检查本目录：11个文件、11,848个事件，ALL OK。另核对每条记录的ID/seq/时间/角色/工具标识、原导出未变化、发布副本与官方脱敏函数结果一致，以及Git blob摘要。

本提交只新增BeaconCat目录，不改队友日志、不自动合入PR。仓库示例清理由团队另行处理。
