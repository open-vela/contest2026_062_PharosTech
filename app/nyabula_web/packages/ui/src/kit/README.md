# Feature Kit（Nk*）

米家式消费级功能组件，供宿主功能页直接使用，也通过 `@nyabula/nyaui-vue` 的 `registerKit()` 注册为 NyaUI DSL 类型（见 `Shared/nyaui/nyaui.md` v1.1 提案）。DSL type 与组件同名去 `Nk` 前缀小驼峰，两处例外：`NkRow` → `settingRow`（v1 `row` 是布局容器）、`NkBanner` → `notice`（v1 已有 `banner`）。

约定：`<script setup lang="ts">`；scoped CSS 只用 `--md-*` / `--radius-*` / `--dur*` / `--ease*` token；可点击目标 ≥ 44px；深浅色皆可；图标走 `UiIcon`，不用 emoji。

```ts
import { NkHeader, NkToggleRow, NkGauge } from '@nyabula/ui';
```

| 组件 | props | emits / v-model | 说明 |
|---|---|---|---|
| `NkHeader` | `icon?, title, subtitle?, tone?('default'\|ok\|warn\|error)` | 默认插槽 = 右侧操作 | 功能页头：大图标圆角块 + 标题 + 状态 |
| `NkTile` | `icon?, title, sub?, value?, active?, layout?('square'\|'wide'), disabled?` | `tap` | 设备卡磁贴 |
| `NkStatTile` | `value, unit?, label, icon?, trend?('up'\|down\|flat), trendText?` | — | 大数值统计 |
| `NkRow` | `icon?, title, sub?, tappable?, disabled?` | `tap`；默认插槽 = 右侧 | 设置行容器 |
| `NkToggleRow` | `v-model:boolean, icon?, title, sub?, disabled?` | `update:modelValue` | 行 + MdSwitch，整行可点 |
| `NkSliderRow` | `v-model:number, title, min?, max?, step?, unit?, iconStart?, iconEnd?, disabled?` | `update:modelValue`(拖动中) `commit`(松手) | 标题 + 数值 + 滑条 |
| `NkSegmentRow` | `v-model:string, title, sub?, items: SegmentItem[], stacked?` | `update:modelValue` | 标题 + 分段控件 |
| `NkChipSelect` | `v-model:string\|string[], options[{id,label,icon?}], multi?, label?, disabled?` | `update:modelValue` | 单/多选 chips |
| `NkGauge` | `value(0-100), label?, unit?('%'), size?(120), stroke?(10), warnAt?, errorAt?, invert?` | — | 270° 环形仪表，按阈值变色 |
| `NkProgressRing` | `value(0-1), size?(96), stroke?(8), indeterminate?, tone?` | 默认插槽 = 环内内容 | 进度环 |
| `NkDial` | `v-model:number(秒), min?, max?, step?, presets?[{label,seconds}], disabled?` | `update:modelValue` | 分:秒 拨盘（+/-、拖动、滚轮、方向键、预设） |
| `NkTimeWheel` | `v-model:string("HH:MM"), minuteStep?(1), disabled?` | `update:modelValue` | 时:分 滚轮（触摸/滚轮/键盘） |
| `NkMediaPlayer` | `title?, artist?, cover?, playing?, position?, duration?, volume?, showVolume?, lyrics?{prev,current,next}, disabled?` | `play, pause, prev, next, seek(秒), volume(0-100)` | 播放器卡 |
| `NkListSection` | `title?, card?(true)` | 默认插槽 + `trailing` 插槽 | 分组列表容器 |
| `NkActionBar` | `primaryText, primaryIcon?, secondaryText?, secondaryIcon?, fixed?, disabled?, busy?, danger?` | `primary, secondary` | 操作条；`fixed` 时贴底并避开 `--shell-bottom` |
| `NkBanner` | `text, title?, tone?('info'\|ok\|warn\|error), icon?, closable?, actionText?` | `close, action` | 提示条 |
| `NkKeyValue` | `items[{key,value,mono?,tone?}], columns?(1\|2)` | — | 键值对列表 |
| `NkColorSwatch` | `v-model:string(#rrggbb), presets?, label?, custom?(true), disabled?` | `update:modelValue` | 预设色点 + 自定义取色 |
| `NkWeatherCard` | `city, temp, unit?('°'), text?, kind?('sunny'\|cloudy\|rain\|snow\|storm\|fog), high?, low?, extra?` | — | 天气卡（内联 SVG 图标） |
| `NkContactList` | `contacts[{id,name,status?,avatar?,online?,actionIcon?}], emptyText?` | `tap(c), action(c)` | 联系人列表 |

示例：

```vue
<NkHeader icon="alarm" title="闹钟" subtitle="下次 07:00" tone="ok" />
<NkListSection title="设置">
  <NkToggleRow v-model="on" icon="notifications" title="启用" sub="每天重复" />
  <NkSliderRow v-model="vol" title="音量" unit="%" icon-start="volume_down" icon-end="volume_up" @commit="save" />
</NkListSection>
<NkTimeWheel v-model="time" :minute-step="5" />
<NkActionBar primary-text="保存" secondary-text="取消" fixed @primary="save" />
```
