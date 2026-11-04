<!-- SPDX-License-Identifier: Apache-2.0 -->

# NyaUI DSL v1（插件 UI 声明规范，双端契约）

插件不画像素，只声明组件树 JSON；Flutter 映射 Widget、Web 映射 Vue3+MD3 组件。
**新组件必须双端同批落地**，本文件组件表是唯一真源。

## 1. 获取与交互（NyaLink topic，见 nyalink.md）

- `req ui.tree {pluginId}` → `res` 组件树（下述格式）。角色 guest 可读。
- `req ui.event {pluginId, componentId, event, values, view?}` → `res {queued:true}`；表示事件已入队，设备端转插件 onCommand。角色 family；旧内置演示保留 `{ok:true}`。
  - `values`：该页当前全部**有 id 的输入组件**的值快照 `{id: value}`（发送端收集）。
- `event ui.patch {pluginId, patches:[{path,value}]}`：path = JSON Pointer（相对树根，如 `/root/children/2/props/value`）。客户端就地打补丁并重渲染；打不上（路径不存在）→ 重新 ui.tree 全量拉。
- `event ui.changed {revision}`：页面或插件状态变化后重新读取列表和已打开页面；读取期间保留当前 iframe，读失败（停止/撤权等）时移除。
- `req plugin.list {}` → `{plugins:[{id,name,version,state,icon,ui:["page"...],permissions:[...]}]}`。

## 2. 树格式

```jsonc
{
  "nyaui": 1,
  "page": { "title": "喂食器", "icon": "pets", "placement": "menu" },  // menu=子页入口 | standalone=独立页
  "root": { "type": "column", "children": [ ... ] }
}
```

节点：`{ "type", "id"?, "props"?, "children"?, "on"? }`
- `id`：有交互/有值的组件必须有，页内唯一。
- `on`：`{ "<event>": { "command": "feed.now", "args": { "$amount": "amount" } } }`——`$xxx` 值引用同页组件 id 的当前值；客户端提交 componentId/event/values；运行时从已发布的 on 绑定重新解析 command 和 $ 参数，不信任客户端提供的 args。附带的 command 如与声明不符则拒绝。

限制（客户端渲染前校验，超限拒渲染显示错误卡）：深度 ≤12、节点 ≤200、字符串 ≤4096、children 仅容器类允许。未知 type → 渲染占位块（灰底 type 名），不崩页。

### 宿主切换多视图

插件可一次发布 `views`，由宿主顶部 UI 选择显示方式：

```jsonc
{ "nyaui": 1, "page": { "title": "陪伴小记" },
  "views": {
    "component": { "nyaui": 1, "root": { "type": "text", "props": { "text": "同一份状态" } } },
    "html": { "nyaui": 1, "html": "<p>同一份状态</p>", "events": {} }
  }
}
```

`views` 与顶层 `root` / `html` 互斥，仅允许 `component`、`html`，不能嵌套 `views`。完整文档仍为 24 KiB 上限。发布包含哪种视图，就必须具有对应的 `ui.page` / `ui.html` 权限。

切换只改变宿主显示选择，不触发插件业务命令。交互请求携带 `view:"component"` 或 `view:"html"`，服务端只解析所选视图已声明的事件。省略 view 时优先组件视图。单视图文档和事件保持原约定。

Web 宿主使用 `NyaUiSurface` 提供顶部切换，或设置 `showSwitcher=false` 并用 `view` 属性接入自己的顶栏。`NyaUiPage` 用于渲染一个已选定的单视图。

## 3. v1 组件表（双端各自实现，props 之外未知字段忽略）

### 布局
| type | props | 说明 |
|---|---|---|
| column | gap(8), padding(0), align("stretch"\|start\|center\|end) | 纵排 |
| row | gap(8), align("center"), wrap(false) | 横排 |
| grid | columns(2), gap(8) | 等宽网格 |
| card | title?, padding(16) | MD3 卡片容器 |
| section | title | 分组标题+内容 |
| spacer | size(8) | 空白 |

### 展示
| type | props |
|---|---|
| text | text, variant("body"\|title\|headline\|label\|caption), color?("primary"\|muted\|error) |
| icon | name(material 图标名), size(24) |
| image | src(仅插件包内 assets 相对路径或 data:URI), height? |
| statCard | label, value, unit?, trend?("up"\|down"\|flat), icon? |
| progress | value(0~1), label?, indeterminate(false) |
| chart | kind("line"\|"bar"), points(number[] ≤120), labels?(string[]), height(120) |
| eye | mode("idle"), height(120)  // 嵌入式猫眼预览（静态表情，不连状态） |
| badge | text, tone("ok"\|"warn"\|"error"\|"info") |
| listTile | title, subtitle?, icon?, trailing?(text)；on.tap 可交互 |
| banner | text, tone("info") |

### 输入（都有 id + value 语义，事件 `change`；button 事件 `tap`）
| type | props |
|---|---|
| button | text, variant("filled"\|"tonal"\|"outlined"\|"text"), icon?, disabled(false) |
| slider | min(0), max(100), step(1), value, label? |
| switch | value(bool), label |
| chips | options(string[]), value(string 或 string[]), multi(false), label? |
| textField | value(""), label?, placeholder?, maxLength(256) |
| colorPicker | value("#62DFAF"), label? |
| dialog | —— v1 不做独立组件；确认类交互用 button + 客户端原生确认（props.confirm: "确认文案" 时客户端先弹原生对话框）|

## 4. 值与 patch 约定

- 输入组件本地即时响应（受控+乐观），change 事件默认**防抖 300ms** 发 ui.event（slider 拖动中不发，松手发）。
- 服务端权威：ui.patch 回来的值覆盖本地。
- placement=menu 的插件在客户端"插件"页显示为入口行（icon+name），点入渲染 page；standalone 后续接入导航（v1 同样按子页渲）。

## 5. 示例（喂食器，Simulator 内置 demo 插件即此）

```jsonc
{ "nyaui": 1,
  "page": { "title": "智能喂食器", "icon": "pets", "placement": "menu" },
  "root": { "type": "column", "props": { "gap": 12, "padding": 16 }, "children": [
    { "type": "row", "props": { "gap": 12 }, "children": [
      { "type": "statCard", "id": "food", "props": { "label": "余粮", "value": "72", "unit": "%", "trend": "down", "icon": "inventory" } },
      { "type": "statCard", "id": "fedToday", "props": { "label": "今日已喂", "value": "3", "unit": "次", "icon": "restaurant" } } ] },
    { "type": "card", "props": { "title": "手动喂食" }, "children": [
      { "type": "slider", "id": "amount", "props": { "min": 5, "max": 50, "step": 5, "value": 20, "label": "出粮克数" } },
      { "type": "button", "id": "feed", "props": { "text": "立即喂食", "variant": "filled", "icon": "play_arrow" },
        "on": { "tap": { "command": "feed.now", "args": { "$grams": "amount" } } } } ] },
    { "type": "card", "props": { "title": "定时" }, "children": [
      { "type": "switch", "id": "sched", "props": { "value": true, "label": "定时喂食" },
        "on": { "change": { "command": "feed.schedule.toggle" } } },
      { "type": "chips", "id": "times", "props": { "options": ["07:00","12:00","19:00","22:00"], "value": ["07:00","19:00"], "multi": true, "label": "时间点" },
        "on": { "change": { "command": "feed.schedule.set" } } } ] },
    { "type": "chart", "props": { "kind": "bar", "points": [3,2,4,3,5,3,2], "labels": ["一","二","三","四","五","六","日"], "height": 120 } } ] } }
```

## 6. Feature Kit 提案与高级 HTML 模式

> Feature Kit 跨端协议仍为提案。2026-10-18 的原生 Core 和 Windows 开发模拟器已接入组件/HTML 发布；Web renderer 已实现，Flutter HTML 通道尚未实现。`nyaui` 版本号仍为 1。页面发布分别要求 `ui.page` 或 `ui.html` 权限，默认拒绝。

### 6.1 Feature Kit 组件类型

米家式消费级组件，type 小驼峰。输入型（★）有 `id + value` 语义，事件 `change`，进入 values 快照；`listSection` 为容器（可带 children）。

| type | kind | props | 事件 / 说明 |
|---|---|---|---|
| header | 展示 | icon?, title, subtitle?, tone?("default"\|ok\|warn\|error) | 功能页头 |
| tile | 展示 | icon?, title, sub?, value?, active(false), layout("square"\|"wide"), disabled(false) | `tap` |
| statTile | 展示 | value, unit?, label, icon?, trend?("up"\|down\|flat), trendText? | 大数值 |
| settingRow | 展示 | icon?, title, sub?, trailing?(text), disabled(false) | 有 on.tap 时可点，`tap` |
| toggleRow ★ | 输入 | icon?, title, sub?, value(bool), disabled(false) | `change` 立即 |
| sliderRow ★ | 输入 | title, min(0), max(100), step(1), unit?, iconStart?, iconEnd?, value, disabled(false) | 松手 `change` |
| segmentRow ★ | 输入 | title, sub?, items[{id,label,icon?}], value(string), stacked(false) | `change` 立即 |
| chipSelect ★ | 输入 | options[{id,label,icon?}] 或 string[], multi(false), label?, value(string 或 string[]), disabled(false) | `change` 立即 |
| gauge | 展示 | value(0~100), label?, unit("%"), size(120), stroke(10), warnAt?, errorAt?, invert(false) | 环形仪表 |
| progressRing | 展示 | value(0~1), size(96), stroke(8), indeterminate(false), tone?("primary"\|ok\|warn\|error), text? | 进度环，text 显示在环内 |
| dial ★ | 输入 | value(秒), min(0), max(5999), step(1), presets?[{label,seconds}], disabled(false) | 分:秒 拨盘；`change` 防抖 300ms |
| timeWheel ★ | 输入 | value("HH:MM"), minuteStep(1), disabled(false) | 时:分 滚轮；`change` 防抖 300ms |
| mediaPlayer | 展示 | title?, artist?, cover?, playing(false), position(秒), duration(秒), volume(0~100), showVolume(true), lyrics?{prev,current,next}, disabled(false) | 事件 `play` `pause` `prev` `next` `seek`(args 合并 `{position}`) `volume`(args 合并 `{volume}`，300ms 合并) |
| listSection | 容器 | title?, card(true) | children |
| actionBar | 展示 | primaryText, primaryIcon?, secondaryText?, secondaryIcon?, fixed(false), disabled(false), busy(false), danger(false) | 事件 `primary` `secondary` |
| notice | 展示 | text, title?, tone("info"\|ok\|warn\|error), icon?, closable(false), actionText? | 事件 `close` `action`（与 v1 `banner` 区分） |
| keyValue | 展示 | items[{key,value,mono?,tone?}], columns(1\|2) | 键值对 |
| colorSwatch ★ | 输入 | value("#rrggbb"), presets?(string[]), label?, custom(true), disabled(false) | `change` 立即 |
| weatherCard | 展示 | city, temp, unit("°"), text?, kind("sunny"\|cloudy\|rain\|snow\|storm\|fog), high?, low?, extra? | 天气卡 |
| contactList | 展示 | contacts[{id,name,status?,avatar?,online?,actionIcon?}], emptyText? | 事件 `tap` `action`，args 合并 `{contact: id}` |

事件 args 合并规则：组件自带载荷（如 seek 的 position、contactList 的 contact）**合并进** `on.<event>.args` 解析结果，同名键以组件载荷为准。

示例（闹钟）：
```jsonc
{ "nyaui": 1, "page": { "title": "闹钟", "icon": "alarm" },
  "root": { "type": "column", "props": { "gap": 12, "padding": 16 }, "children": [
    { "type": "header", "props": { "icon": "alarm", "title": "闹钟", "subtitle": "下次 07:00", "tone": "ok" } },
    { "type": "timeWheel", "id": "time", "props": { "value": "07:00", "minuteStep": 5 },
      "on": { "change": { "command": "alarm.set", "args": { "$at": "time" } } } },
    { "type": "listSection", "props": { "title": "设置" }, "children": [
      { "type": "toggleRow", "id": "on", "props": { "icon": "notifications", "title": "启用", "value": true },
        "on": { "change": { "command": "alarm.enable" } } },
      { "type": "sliderRow", "id": "vol", "props": { "title": "音量", "unit": "%", "value": 60, "iconStart": "volume_down", "iconEnd": "volume_up" },
        "on": { "change": { "command": "alarm.volume" } } } ] },
    { "type": "actionBar", "props": { "primaryText": "保存", "secondaryText": "取消" },
      "on": { "primary": { "command": "alarm.save" }, "secondary": { "command": "alarm.cancel" } } } ] } }
```

### 6.2 高级 HTML 模式（插件自绘）

`ui.tree` 可返回 HTML 模式：
```jsonc
{ "nyaui": 1, "page": { "title": "..." },
  "html": "<button onclick=\"nyabula.send('refresh')\">刷新</button>",
  "events": { "refresh": { "command": "refresh" } } }
```
- `html` 与 `root` 互斥；完整发布文档上限 24 KiB。CSP 允许内联脚本/样式和 data: 图片，阻止外链资源与 fetch/XHR。
- 宿主用 `<iframe sandbox="allow-scripts" srcdoc>` 承载（无 same-origin、无顶层导航和表单提交权限），注入：
  1. `<style>:root{--md-* / --radius-* / --dur* / --ease* / --font-* ...}</style>`（宿主当前主题 token）；
  2. SDK 脚本，暴露 `window.nyabula`。
- 权限：HTML 模式需插件声明 `permissions: ["ui.html"]`，设备主人可以撤销。
- 宿主画布透明，默认按内容自动增高和缩短；SDK 监听布局、内容、字体和宽度变化。插件自身的卡片仍可使用主题颜色。主题消息只更新 CSS 变量，不重新加载 iframe。
- `events` 是允许执行的事件表，服务器据此派发 onCommand；未声明事件拒绝。

**SDK（帧内）**
```ts
window.nyabula = {
  pluginId: string,
  theme: Record<string,string>,                       // 当前 token 快照，会随主题更新
  send(event: string, opts?: { componentId?, command?, args?, values? }): void,
  onPatch(cb: (patches: {path,value}[]) => void): () => void,
  onTheme(cb: (vars: Record<string,string>) => void): () => void,
};
```

**消息桥协议**（postMessage，信封必带 `nyaui: 1`）
| 方向 | 消息 | 说明 |
|---|---|---|
| 帧→宿主 | `{nyaui:1, type:"resize", height:number}` | 内容高度；宿主校验来源和有限数值后更新 iframe 高度 |
| 帧→宿主 | `{nyaui:1, type:"ready"}` | SDK 就绪；宿主此后才转发 patch |
| 帧→宿主 | `{nyaui:1, type:"event", componentId?, event, command?, args?, values?}` | 宿主原样映射为 `ui.event`（与组件树模式同一 payload 形状） |
| 宿主→帧 | `{nyaui:1, type:"patch", patches:[{path,value}]}` | `ui.patch` 透传，插件自行应用（路径语义由插件定义） |
| 宿主→帧 | `{nyaui:1, type:"theme", vars:{"--md-primary":"#..."}}` | 主题切换；SDK 已自动写入 `:root` 并回调 |

宿主只接受 `ev.source === iframe.contentWindow` 的消息；帧内消息 `origin` 为 `null`（sandbox），双方 targetOrigin 用 `*`，帧内也校验消息来自 parent；业务授权由原生/模拟器运行时执行。

Web 实现：`packages/nyaui-vue/src/components/NyaHtmlFrame.vue`、`packages/nyaui-vue/src/html-sdk.ts`。Flutter 端建议 `webview_flutter` + `JavaScriptChannel` 等价实现。
