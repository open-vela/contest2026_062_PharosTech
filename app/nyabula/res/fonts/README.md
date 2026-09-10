# 字体准备与 Noto Sans 回退

Git只保存生成脚本、声明头和许可说明，不保存完整字体或生成字形表。
字体放本目录；生成文件在 `src/generated/fonts/`，两者均被Git忽略。

## 一条命令生成

在队伍仓根目录执行（需要Python 3、Node.js/npm）：

```sh
python3 app/nyabula/tools/generate_fonts.py --download-fallback
```

逐类检查原字体，有文件即使用，没有则回退到Noto Sans CJK SC Bold：

| 类别 | 本地文件名 | 生成字号 |
| --- | --- | --- |
| 标题 | AlimamaShuHeiTi.ttf | 20、28、42 |
| 正文 | MiSans-Semibold.ttf | 14、18 |
| 英文/数字 | Tinos-Bold.ttf | 18、42、72、96、119 |

原字体由部署者从官方渠道下载并遵守各自许可；文件名不同时按上表命名。
来源：[阿里巴巴字体](https://fonts.alibabagroup.com/)、
[MiSans官方](https://hyperos.mi.com/font/zh/download/)、
[Tinos官方仓](https://github.com/googlefonts/tinos)。
MiSans/阿里妈妈字体不因用于本项目而变成Apache-2.0，不随源码仓再分发。

`--download-fallback`仅从Noto官方仓下载固定版本并核验SHA-256；
不替客户接受其他字体许可，不从非官方镜像获取字体。
CI没有原字体，按同一脚本从官方源获取Noto Sans后生成和编译。

已有字体文件时可离线使用（lv_font_conv需预先安装或缓存）：

```sh
python3 app/nyabula/tools/generate_fonts.py \
  --font-dir /path/to/original-fonts \
  --fallback-font /path/to/NotoSansCJKsc-Bold.otf
```

`--fallback-font`也可指定经授权的其他字体文件，补齐缺失类别的全部字号。
`--output-dir`指定生成目录；`--html`增加场景外文案的字形。
默认收集Eye的C源码与公共头里的中文和必要标点，加ASCII字形。
这是有限子集，不保证任意输入中文字形；新增文案后重新运行。
固定使用`lv_font_conv@1.5.3`，记录字体/字符集哈希与字号；输入不变跳过重建。

## 运行时完整字体

可选启用`CONFIG_CONTEST2026_062_NYABULA_DYNAMIC_FONTS`，将三份完整原字体
放入`CONFIG_CONTEST2026_062_NYABULA_FONT_ROOT`（默认`/data/nyabula/fonts`）。
FreeType加载失败时使用编译子集；CI默认子集就是Noto Sans。
本PR的K7配置默认关闭动态字体，接屏后可按存储部署情况另行启用。

分发含Noto字形的固件时保留`NOTICE-NOTO-SANS.txt`与`LICENSE-NOTO-SANS.txt`。
其他字体保留其对应许可和署名，并自行核对分发条件。
