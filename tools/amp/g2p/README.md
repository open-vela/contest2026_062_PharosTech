# nyamp g2p

MeloTTS zh_en 的文本前端（文本归一化 + 分词 + G2P + 分句），纯 C++17 标准库，
无第三方依赖，给 Linux aarch64 计算域守护进程用（目标机没有 Python）。

输入 UTF-8 文本，输出若干 `Utterance`：`phonemes` / `tones` 两条 int64 序列，
直接就是 ONNX 前缀模型的 `x` / `tones` 输入。

## 模型资产（不入库）

资产是第三方模型文件，**不提交到仓库**，测试和 CLI 通过参数或环境变量定位。

- 来源：sherpa-onnx 发布的 `vits-melo-tts-zh_en.tar.bz2`
  （`https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models`），
  由 `https://github.com/myshell-ai/MeloTTS` 转换而来。
- 许可：包内 `LICENSE`（MIT，Copyright (c) 2024 MyShell.ai）。
  `dict/` 是 cppjieba 词典（上游 cppjieba 为 MIT，包内 `dict/` 没有单独许可文件，
  只有 `dict/README.md`）。
- 本前端只读下面三个文件；`*.fst`、`model*.onnx`、`dict/` 其余文件都不需要。

| 文件 | 必需 | sha256 |
|---|---|---|
| `tokens.txt` | 是 | `d18664a7e12bd7ea1022ddaf951e534e136815016c5a809d6b64156bffb4369d` |
| `lexicon.txt` | 是 | `7236884b02435ac5d10cf69b4be40a61b45aa676b5300f0e412f185748fee528` |
| `dict/jieba.dict.utf8` | 否（缺则退回正向最大匹配） | `3043b77068e09c9904f27cad82f12b6ebe9dbdb5aeff3b25e45ab7f9c1122b55` |
| `LICENSE` | 随资产分发 | `88a50e5a02bbc2a5c2f084dc19da751aa97b1690f5fda76cd8005c8634d1ca70` |

金标准夹具（`real_fixture.py` 产物，同样不入库，ids 已内嵌在测试里）：

| 文件 | sha256 |
|---|---|
| `X.BIN` | `9d78820f4c13fab14f6535a80a4bca27751f9126c6f7c90b1251528a8bbd3345` |
| `TONES.BIN` | `79217a52d0af0789f021629a7498368e9df7fcd38d1d0626608b23ec4f337770` |

## 构建与测试

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
NYAMP_G2P_ASSETS=/path/to/vits-melo-tts-zh_en \
NYAMP_G2P_GOLDEN=/path/to/real-fixture ctest --test-dir build -V
# 或者: ./build/nyamp_g2p_test <assets-dir> [<golden-dir>]
./build/nyamp-g2p --names --stats <assets-dir> "你好，现在是15:30。"
```

没给资产时测试只跑不依赖资产的归一化用例并返回 77，CTest 显示 Skipped 而不是假通过。

目标机编译检查（只编库）：

```
cmake -S . -B build-arm64 -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DNYAMP_G2P_TOOLS=OFF
cmake --build build-arm64 -j2
```

## 约定（从夹具和导出脚本推出，置信：实测确认）

依据：`real_fixture.py`（产出 X.BIN/TONES.BIN 的脚本）+ 夹具解码 + sherpa-onnx 1.13.8
debug 日志（模型元数据 `add_blank=1`）。

- **blank**：`x = [0]`，之后每个符号后跟一个 0，长度 `2n+1`。首尾都是 blank。
  `tones` 完全同构（blank 处 tone=0）。
- **标点**：`，`→`,`(106)，`。`→`.`(107)，`？`→`?`(104)，`！`→`!`(103)，tone 恒为 0。
  `、：；` 也读作 `,`；`……`/`...`→`…`；`—～~`→`-`。引号、括号、markdown 装饰符静默跳过
  （sherpa-onnx 同样不发声；MeloTTS Python 会映射成 `'`，这里为省帧预算不采用）。
- **声调**：中文 1–5（5=轻声），英文用 lexicon 里的 7–10，直接照抄 lexicon，不加偏移
  （元数据 `tone_start=0`）。
- **没有 language id 向量**：导出的模型输入只有
  `x, x_lengths, tones, sid, noise_scale, length_scale, noise_scale_w`，语言 id 已烘进图里。
- 夹具是**一个 utterance**（三句合一，47 符号 → 95 token → 342 帧），不是三个。

## 分词

| 模式 | 行为 | 用途 |
|---|---|---|
| `kFewestWords`（有 jieba 词典时默认） | jieba 词典 ∪ lexicon 上取词数最少的切分，平局用 jieba 词频定 | 金标准逐位一致 |
| `kForwardMaxMatch` | lexicon 上 10 字窗口正向最大匹配 | 与 sherpa-onnx ≥1.12（phrase-matcher）一致 |

实测对照（置信：实测确认）：

- 正向最大匹配把 `要不要` 切成 `要不/要`（bu4），金标准是 `要/不要`（bu2）；
  FMM 模式与金标准**仅**差这一个音节（tone 下标 65、67）。sherpa-onnx 1.13.8 自己也是 bu4。
- 纯 jieba 最大概率切分试过并弃用：它把 `不对` 切成 `不`+`对`，丢掉 lexicon 里的变调（bu2）。
- FMM 模式与 sherpa-onnx 1.13.8 debug 输出逐符号一致（15 句里 13 句全等；另 2 句差异是有意的：
  `openvela`/`wifi` 别名，以及 `……` 合并为一个 `…`）。

## 分句与帧预算

先按 `。！？；!?;` 和换行切句，句子超预算再按 `，,、：` 切，仍超再按词切
（`split_at_words`，可关）。**永不截断**：切不动的超预算片段整段返回并置 `over_budget=true`，
由调用方拒绝。相邻短句在预算内合并成一个 utterance（`merge_sentences`，可关）。

预算单位是符号数（不含 blank）：`budget = max_frames_hint / frames_per_symbol`，
默认 `9.0` → 512 帧桶对应 **56 符号**。依据（主机 onnxruntime 跑原模型，
`length_scale=1`，本导出对同一输入帧数确定）：

| 符号数 | 帧数 | 帧/符号 |
|---|---|---|
| 3 / 5 | 35 / 46 | 11.67 / 9.20（短句有约 20 帧固定开销，离上限很远） |
| 32 / 34 / 37 | 258 / 273 / 298 | 8.06 / 8.03 / 8.05 |
| 45–55（11 句） | 324–410 | 6.94–7.84 |
| 64 / 65 / 68 / 76 | 459 / 441 / 512 / 535 | 7.17 / 6.78 / 7.53 / 7.04 |

56 符号按最坏实测 7.84 算是 439 帧，留 14% 余量；68 符号已经正好 512、76 符号溢出。
这只是启发式：**权威判据是前缀模型实际输出的 latent 长度**，调用方仍应在
`T > 512` 时拒绝。`length_scale`（语速）改了要同比例改 `frames_per_symbol`。

## 资源占用（构建机 x86_64，g++ 14 -O2，置信：实测确认）

| 模式 | 加载 | 峰值 RSS |
|---|---|---|
| lexicon + jieba（默认） | 约 180–260 ms | 32 MB |
| 仅 lexicon（FMM） | 约 120 ms | 21 MB（稳态 12 MB） |

`Process()` 单句 < 0.1 ms。

## 未覆盖（已知限制）

- **多音字消歧**：只靠 lexicon 词条；`new_heteronym.fst` 没用（实测 sherpa-onnx 对 melo
  也只是插入 `#$|行长|$#` 标记然后当 OOV 丢掉）。单字多音（`长`/`行`/`得`/`地`）取 lexicon 首读音。
- **变调**：lexicon 多字词自带变调；单字 `一`/`不` 的变调是可选项（`yi_bu_sandhi`，默认关，
  因为金标准和 sherpa-onnx 都不做，`一天` 是 yi1）。三声连读变调、儿化、轻声规则都不做。
- **jieba HMM 新词发现**不做（对 G2P 无影响：未登录词反正逐字读）。
- **数字**：一律按中文读，英文语境（`3 cats`）也是；区间/比分 `3-5`、`3:2` 读成停顿而不是
  “到/比”；`M/D` 日期（`9/20` 会读成分数）、12 小时制 `3:15pm`、罗马数字、科学计数法、
  电话号码的“幺”、`2` 读“两”只覆盖常见量词。
- **单位**只有一张小表（km m cm mm kg g mg ml L s ms min h Hz kHz MHz GHz V W kW mA mAh dB ℃ ° % ‰
  ¥ $ € £）；`GB/MB`、`km²` 等按字母读。
- **英文**：lexicon 查词 → 连字符/撇号拆分 → 三字母以上词根复合拆分 → 逐字母拼读；
  2–4 字母全大写当缩写拼读（`NASA` 也会被拼读）；无英文数字、无缩写点号处理（`e.g.`）。
- **符号**：URL、邮箱、`@ & / \`、emoji、生僻字/lexicon 没有的汉字一律丢弃并计数
  （`Utterance::dropped`、`dropped_total()`），不报错。
- 日文/韩文等其他文字丢弃。
