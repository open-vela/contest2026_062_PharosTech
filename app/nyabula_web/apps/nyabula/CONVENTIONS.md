# apps/nyabula 页面开发约定（agent 必读）

## 结构
- 每个页面 = `views/<domain>/<Page>.logic.ts`（composable：数据+动作，三端共用）+ 变体 `<Page>.desktop.vue` / `<Page>.tablet.vue` / `<Page>.phone.vue` + 入口 `<Page>.vue`（只含 `<LayoutSwitch :desktop :tablet :phone />`）。变体缺省回退相邻端，但**结构差异大的页面必须三写**；禁止一写靠媒体查询硬撑。
- 路由已在 `src/router/index.ts` 定义，props 由路由传入（`key`、`id`、`type`、`section`）。变体组件用 `defineProps` 接收同名 props（可选）。
- 桌面右侧栏：`import ContextSlot from '../../components/ContextSlot.vue'`，`<ContextSlot>...</ContextSlot>` 内容会传送到桌面壳右栏；其他壳内联渲染（fallback）。
- form factor：`inject<ReturnType<typeof useFormFactor>>('formFactor')`，可读 `orientation`（平板横竖屏）。

## 数据
- 会话：`useSessionStore()`（`request(topic,data)`、`onEvent`、`connected`、`role`、`isOwner`、`canControl`、`deviceKey`、`device`）。
- 插件：`usePluginsStore()`（`list`、`refresh`、`loadTree(id, force?, surface?)`、`trees`、`treeVersion`、`sendEvent`、`setPermission`）。
- 眼睛：`useEyeStore()`（`lastState`、`activeMode/activeScene`、`setMode/setScene/toggleScene/look`、`SCENE_META`、`MODE_LABELS`、`SCENE_GROUPS` 从 `stores/eye.ts` 导出）。
- 账号：`useAccountStore()` + `api/cloud.ts`。
- 异步：`useAsyncTask(fn, { errorPrefix, holdRoute: true, immediate: true })`（`composables/useRequest.ts`）——首屏数据用 `holdRoute` 让路由幕布等数据。

## 反馈（禁止原生 alert/confirm/prompt）
- `useToastStore().ok/warn/error(e, '前缀')`；`useDialogStore().confirm(msg, {title, danger, confirmText})` 返回 Promise。
- 空态/错误态：`<EmptyState icon title hint actionText @action />`（`tone="error"`）。加载：`<Skeleton :lines="3" />`。
- 契约已定但设备未实现的功能：UI 照做，加 `<span class="contract-only">契约预留</span>` 标签，不要藏。

## 组件（来自 `@nyabula/ui`）
`MdButton(variant filled|tonal|outlined|text|icon)`、`MdCard(title)`、`MdChip(selected)`、`MdSlider`、`MdSwitch(v-model,label)`、`MdTextField(v-model,label,placeholder,icon,error,hint,type)`、`SegmentedTabs(v-model,items[{id,label,icon,badge}],stretch)`、`BottomSheet(open,title,side)`、`UiIcon(name,size)`、`StatusPill`、`ImageViewer`、`EmptyState`、`Skeleton`。
- 图标名见 `packages/ui/src/icons.ts`（Material 命名）+ eye-engine 场景图标。
- 全局 class（`src/styles/app.css`）：`.page .page.narrow .page-title .page-sub .section-title .grid-cards .kv .list-tile(.tile-icon/.tile-body/.tile-title/.tile-sub/.tile-trail) .tag(.ok/.warn/.err/.info) .stack .row(.between/.wrap) .muted .mono .contract-only`。
- 手机变体：次级操作用 `BottomSheet`，列表用 `.list-tile`，大按钮固定底部时避开 `var(--shell-bottom)`。平板变体：横屏主从双栏、竖屏单栏栈（用 `orientation`）。桌面：多栏 + `ContextSlot`。

## 代码风格
- `<script setup lang="ts">`，注释英文，中文文案直接写在模板。scoped CSS 用 `--md-*`、`--radius-*`、`--dur*`、`--ease*` token，不写死颜色。
- 通过 `npx vue-tsc --noEmit -p tsconfig.json`（在 apps/nyabula 下）零错误。
- 不新增依赖，不用 CDN，不用 emoji 当图标。
