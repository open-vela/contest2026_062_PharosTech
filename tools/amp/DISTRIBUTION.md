# 分发与 OTA 约定 v1

本文定义 Nyabula K7 的**分发包格式**、**安装流程**和**许可边界**。
工具是 `tools/amp/resources/nyamp_bundle.py`，板端执行者是 `nbootctl`。

不是发布授权：本文描述的机制可以在本地任意使用，**对外分发仍需人工核对许可**。

## 为什么分三层

三层的体积、变更频率和许可复杂度都不同，混在一起会让每次改模型都要重刷固件。

| 层 | 包 kind | 典型体积 | 变更频率 | 内容 |
|---|---|---:|---|---|
| 引导链 | `boot` | ~8 MB | 极低 | MiniLoader、N-Boot FIT、trust（ATF+OP-TEE）、bootctrl |
| 计算域 | `firmware` | ~40 MB | 低 | 主域 openvela、AMP FIT（Linux+DTB+initramfs+A53 openvela）|
| 模型 | `model` | 几十~几百 MB | **高** | 量化模型、tokenizer、lexicon |
| 运行库 | `runtime` | ~55 MB | 中 | onnxruntime、rknn、rkllm、sherpa |

**为什么模型必须独立**：量化过的 1B LLM 有几百 MB，而固件只有 40 MB。
把模型塞进固件意味着每次换模型都要重下整个固件。

**为什么运行库独立于模型**：运行库随框架版本变，模型随训练变，两者不同步。
但它们**同属计算域兼容性契约**——`model`/`runtime` 类包必须声明依赖的
runtime 精确版本（见下），所以换运行库时模型包会因不兼容被拒，不会静默跑错。

## manifest.json 契约

一个包 = 一个目录 + 一个 `manifest.json`。归档内 `manifest.json` 在首位，
按排序键、UTF-8、无多余空白规范化。

```json
{
  "schema_version": 1,
  "bundle_id": "nyabula-fw-20260919",
  "version": "1.0.0",
  "kind": "firmware",
  "redistribution": "review-required",
  "compatibility": {
    "soc": "rk3576", "architecture": "aarch64", "os": "nuttx",
    "interface_version": 1, "dependencies": {}
  },
  "provenance": {
    "source": "KICKPI-K7 team build 20260918",
    "revision": "contest2026_062_PharosTech@b78adfb2",
    "conversion": "none"
  },
  "license_files": ["licenses/NOTICE.txt"],
  "files": [
    { "path": "nuttx-main.bin", "bytes": 4752792, "sha256": "b275ba...",
      "install": { "partition": "nuttx_a" } },
    { "path": "miniloader.bin", "bytes": 360448, "sha256": "0ee14d...",
      "install": { "lba": 64, "sectors": 704 } },
    { "path": "licenses/NOTICE.txt", "bytes": 522, "sha256": "14ad25..." }
  ]
}
```

### 字段规则

| 字段 | 规则 |
|---|---|
| `schema_version` | 整数 1 |
| `bundle_id` / `version` | 稳定 ASCII；版本**禁止** `latest` |
| `kind` | `model`、`runtime`、`fixture`、`firmware`、`boot` |
| `compatibility` | `soc`/`architecture`/`os` 非空；`interface_version=1`；`dependencies` 精确版本，禁止 `latest` |
| `provenance` | `source`/`revision`/`conversion` **三者均非空** |
| `redistribution` | 必须为 `approved` 才可打包，除非显式 `--allow-review-required` |
| `license_files` | ≥1 个，必须在 `licenses/` 下，**且**列入 `files` 并校验哈希 |
| `files[].sha256` | 小写 64 位十六进制 |
| `files[].install` | 见下 |

### install 字段

**只有 `firmware`/`boot` 的载荷文件需要**，许可文件不要（它不写进介质）。

两种形式，**二选一，不能同时**：

```json
"install": { "partition": "nuttx_a" }         分区名
"install": { "lba": 64, "sectors": 704 }      绝对 LBA 区间
```

规则：

- `partition` 必须是 nbootctl 认识的名字（`uboot`、`trust`、`bootctrl`、
  `nuttx_a`、`nuttx_b`、`amp_a`、`amp_b`、`data`）。
  **不认识的名字在打包时就被拒**——否则要传完整个文件才在板上失败。
- `lba` 形式必须给 `sectors`，且 `sectors*512 ≥ bytes`。
  **装不下也在打包时被拒**，不让板子去处理溢出。
- **为什么要有 lba 形式**：MiniLoader 在 sector 64，不属于任何 GPT 分区。

**歧义为什么必须消除**：`partition` 和 `lba` 同时给出时，板子只能猜，
猜错就写坏别的东西。宁可在打包时报错。

### 可复现性

同一份 staging 目录重复打包得到**逐字节相同**的归档（固定所有权、时间戳、
gzip mtime=0、排序键）。已验证：两次打包 SHA256 一致。

不能由这个哈希推断发布者身份；清单应从可信的版本化来源获取。

## 安装流程

摘要**两端各算一次**，任何不一致都在写盘前中止。

```
浏览器                          板子
  │
  ├─ 1. 读文件
  ├─ 2. crypto.subtle.digest('SHA-256', buf)
  │     （WebCrypto 原生，无 JS 依赖）
  │
  ├─ 3. POST /ota/begin ──────────→ 记录 (目标, 字节数, 摘要)
  │      {partition|lba, size, sha256, confirm}
  │
  ├─ 4. POST /ota/upload ─────────→ 边收边写 /tmp/ota.bin，边算 SHA-256
  │
  ├─ 5.                ←─────────── {received, sha256}
  │   比对两端摘要
  │     不符 → 丢弃，中止（一个扇区都没写）
  │     相符 ↓
  │
  └─ 6. POST /ota/commit ─────────→ 从 /tmp 读
                                     写目标
                                     回读比对
                                     报结果
```

**为什么不在浏览器里算 MD5**：WebCrypto 故意不含 MD5，需要引入 JS 库。
而 SHA-256 是 WebCrypto 内置的，且板上 bootctrl 已经在用
`crypto/sha2.h`——复用现成实现，两端都不新增依赖。

### 板端两道校验

`nbootctl write-part` / `write-raw` / `write-gpt` 每次都做两次比对：

| 比对 | 抓什么 |
|---|---|
| 源文件 vs 期望摘要 | 字节有没有完整到达 |
| 介质回读 vs 源文件 | 字节有没有完整落盘 |

**两者不可互相替代**：前者是链路问题（坏线、中断），后者是存储问题（坏块）。

**摘要不符时一个扇区都不碰**——检查在 `open()` 介质之前完成。

### 为什么不用流式直写

流式写入时摘要还算不出来，等算出来时数据已经落地。中途断线留下的是
**半写的分区**。改成"先落文件、两端对摘要、再整块写"，中途任何失败都
只影响 `/tmp`。

### 为什么没有硬护栏

板子本身有 MaskROM、Loader 和 RKUSB，写坏可以救回来。硬护栏防不住
真实事故（有人就是要刷引导链），只会挡住正当操作。

**防护靠这些**：

1. 摘要双端比对 —— 防传输损坏
2. 目标二选一明确 —— 防写错地方
3. 写入前检查容量 —— 防溢出
4. 写入后回读 —— 防坏块
5. WebUI 的二次确认 —— 防手滑

第 5 条是**操作层**的，不是代码层的：用户在界面上确认目标，不是工具禁止目标。

## 许可

`redistribution` 字段是**人工核对过**才算 `approved`，**工具不会自动批准**。
当前的核对状态见 `tools/amp/resources/dependencies.json`。

打包未批准的包需要显式标志：

```sh
python3 tools/amp/resources/nyamp_bundle.py pack STAGING \
  --output out.tar.gz --allow-review-required
```

这个标志的存在是为了让内部测试镜像能打出来，而**不是**假装手续办完了。
命令行里出现它就说明某个东西在许可核对之前被打包了。

**闭源 runtime 可执行不等于工具链整个 SDK 都能打包**；平台库也不能
被一律改标 Apache。

### 当前缺口

`dependencies.json` 逐项记录。已知需要处理：

- Rockchip RKNN / RKLLM runtime 再分发权未确认
- 部分模型的上游 revision 未固定（`source_revision: null`）
- 转换脚本 revision 未保留
- GPL/LGPL 组件的对应源码材料

## 不属于发布资源

以下内容**不进包**（现有 resources 文档已确立，此处重申）：

- 私人录音、主人模板、声纹
- 聊天记录 / KV 缓存
- 开发过程中的临时镜像与排错候选

## 工具

```sh
# 校验一个 staging 目录
python3 tools/amp/resources/nyamp_bundle.py verify STAGING \
  --profile tools/amp/resources/dependencies.json

# 打包（firmware/boot 不需要 profile）
python3 tools/amp/resources/nyamp_bundle.py pack STAGING \
  --output out.tar.gz --allow-review-required

# 测试
python3 -m unittest discover -s tools/amp/resources -p 'test_*.py'
```

`model`/`runtime` 打包**必须**提供 profile：目标芯片、架构、OS、接口版本
及声明的 runtime 版本须精确匹配。`firmware`/`boot` 不需要——它们的兼容性
是板子，不是模型接口。

staging 必须是**独立的本地目录**：复制已核对的文件，写 manifest；
不能把整个开发目录直接打包。工具只收录 manifest 列出的文件，拒绝软链接、
路径穿越、重复路径、输出覆盖、哈希/大小不符。
