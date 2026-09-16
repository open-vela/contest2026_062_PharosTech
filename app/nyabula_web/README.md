# Nyabula Web

Nyabula 的浏览器控制面板，包含桌面、平板和手机布局。它通过 NyaLink
连接设备端 `nyabula_web` WebSocket 服务，浏览器不是计时、天气、简报或
主动陪伴任务的状态真源。

## 构建

```sh
corepack enable
pnpm install --frozen-lockfile
pnpm build
pnpm typecheck
pnpm test
```

生产构建入口为 `apps/nyabula`。`apps/panel` 与 `apps/console` 保留为协议和
组件调试面板。

## 字体

仓库不分发 MiSans 或阿里妈妈数黑体文件。推荐按以下顺序在开发机或最终部署
环境中安装字体，浏览器会自动使用系统字体，不需要把字体文件复制进仓库：

| 用途 | 推荐字体 | 获取与许可 |
|---|---|---|
| 标题、数字和强调信息 | 阿里妈妈数黑体（Alimama ShuHeiTi） | 从阿里妈妈官方或其明确授权渠道获取；安装前保留并核对随字体提供的授权声明 |
| 正文首选 | MiSans | [小米字体官方网站](https://hyperos.mi.com/font/en/download/)；其许可禁止把字体软件作为本仓组件再次分发，只建议在本机安装 |
| 正文开源替代 | 思源黑体（Noto Sans CJK SC / Source Han Sans SC） | [Adobe Source Han Sans](https://github.com/adobe-fonts/source-han-sans)，SIL Open Font License 1.1 |
| Windows 回退 | Microsoft YaHei | 随系统提供，不进入仓库 |

CSS 字体栈是：标题优先 `Alimama ShuHeiTi`，正文优先 `MiSans`；未安装时依次
回退到 `Noto Sans CJK SC`、`Microsoft YaHei` 和通用 sans-serif。公开发布前仍须
按实际部署方式复核每种字体的最新许可；不要提交 `.ttf`、`.otf` 或字体子集产物。
