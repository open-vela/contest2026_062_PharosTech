# 计算资源清单与资源包 v1

**分发格式、安装流程与许可边界的完整约定见
[../DISTRIBUTION.md](../DISTRIBUTION.md)。** 本文只讲资源包这一层。

`dependencies.json` 是2026-09-14已测文件的依赖清单，不是已经获准公开分发的release。
模型、vendor runtime与基础动态库各自保留来源、版本、SHA256、许可状态和发布缺口。
`source_revision: null`表示历史转换记录未固定到准确上游revision，不用当前HEAD冒充原版本。
这不影响本地已知hash文件复测，但必须补齐后才能形成正式发布包。

## 资源布局

- 模型与runtime分包，固件另行分发；仓库只存清单、适配代码和转换/打包工具。
- 模型包：`models/llm/`、`models/asr/`或`models/tts/`，另带tokenizer/lexicon和license。
- runtime包：`bin/`放服务可执行程序，`runtime/rkllm/`、`runtime/speech/`、
  `runtime/rknn/`分开存vendor库；平台libc/loader按固件profile匹配，不覆盖主机系统库。
- 未核实分发条款的库保持review-required，最终可以由官方来源获取，不能自行改标Apache。
- 私人录音、主人模板、声纹、聊天/KV缓存不属于发布资源。

本轮提供本地verify与确定性pack，不实现联网下载、安装、分区写入、升级或自动发布。
Release上传是单独的用户授权动作。

## manifest.json字段

| 字段 | 要求 |
|---|---|
| schema_version | 整数1 |
| bundle_id、version | 稳定ASCII标识，禁止latest |
| kind | model、runtime或fixture（测试数据，不能冒充模型） |
| compatibility | soc、architecture、os、interface_version=1、dependencies精确ID→版本 |
| provenance | source、revision、conversion，均须非空；转换方式或none |
| redistribution | 必须approved，表示已人工完成该包的许可核对；工具不会自动批准 |
| license_files | 至少一个licenses/下文件，同时列入files并校验hash |
| files | 每个文件的path、bytes、sha256，小写SHA256，包内相对路径 |

`files`不列manifest自身。归档内manifest自动以排序键、UTF-8、无多余空白方式规范化。
所有权/时间戳固定，gzip时间戳为0，同一内容重复打包得到同一SHA256。
不能由这个hash推断发布者身份；清单应从可信的版本化release获取。

## 使用

准备独立本地staging目录：复制已核对的文件，写manifest；不能把整个开发目录直接打包。
只收录manifest列出的文件，拒绝软链接、路径穿越、重复路径、已有输出覆盖与hash/大小不符。
staging必须由调用者独占，不能在另一个进程同时修改文件时打包。

```sh
python3 tools/amp/resources/nyamp_bundle.py verify /path/to/staging \
  --profile tools/amp/resources/dependencies.json
python3 tools/amp/resources/nyamp_bundle.py pack /path/to/staging \
  --profile tools/amp/resources/dependencies.json \
  --output /path/to/nyabula-model-v1.tar.gz
python3 -m unittest discover -s tools/amp/resources -p 'test_*.py'
```

模型/runtime打包必须提供profile，目标芯片、架构、OS、接口版本及声明的runtime版本须精确匹配。
`verify`不带profile只验证包内容，不代表目标机兼容；安装端仍需核对实际固件profile。
fixture打包只用于测试，不需要目标硬件。

## 当前发布缺口

已经固定转换后LLM/ASR/TTS文件与主要运行库hash，基础glibc/GCC版本来自构建机包查询。
配套tokenizer/lexicon也已记录当前留存文件的hash；仍须恢复准确源模型revision，保留转换脚本revision，以及补齐
第三方版权/许可和对应源码义务材料。`dependencies.json`逐项列出，不因测试通过自动清零。
闭源runtime可执行不等于工具链整个SDK都能打包，平台库也不能被一律改标Apache。
