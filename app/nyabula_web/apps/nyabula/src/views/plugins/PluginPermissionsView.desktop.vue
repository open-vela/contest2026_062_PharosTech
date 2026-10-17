<script setup lang="ts">
/* Desktop: two columns — switch list left, explanation of the focused
 * permission right; bulk actions in the context panel. */
import { computed, ref } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon, permissionIcon, permissionZh } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import PermissionRow from './PermissionRow.vue';
import { usePluginPermissionsView } from './PluginPermissionsView.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPermissionsView(props);
const focus = ref<string | null>(null);
const focused = computed(() => v.perms.value.find((x) => x.name === focus.value) ?? v.perms.value[0] ?? null);
const RISK_LABEL = { high: '高敏感', mid: '中等', low: '低' } as const;
const RISK_TONE = { high: 'err', mid: 'warn', low: 'ok' } as const;
</script>

<template>
  <div class="page perms-desktop">
    <div class="row between">
      <div>
        <h2 class="page-title">{{ v.plugin.value?.name ?? v.id.value }} · 权限</h2>
        <p class="page-sub">已授 {{ v.summary.value.granted }}/{{ v.summary.value.total }}<span v-if="v.disabledHint.value"> · {{ v.disabledHint.value }}</span></p>
      </div>
      <MdButton variant="text" @click="v.openPlugin(v.id.value)"><span class="row" style="gap: 4px"><UiIcon name="widgets" :size="18" /> 打开插件页</span></MdButton>
    </div>

    <div v-if="v.loading.value && !v.plugin.value" class="stack"><Skeleton v-for="i in 5" :key="i" height="60px" radius="var(--radius-m)" /></div>
    <EmptyState v-else-if="v.errorText.value && !v.plugin.value" tone="error" title="加载失败" :hint="v.errorText.value" action-text="重试" @action="v.retry" />
    <EmptyState v-else-if="v.notFound.value" icon="extension" title="未找到该插件" hint="它可能已被卸载" action-text="返回列表" @action="v.openList" />
    <div v-else class="cols">
      <section class="stack">
        <EmptyState v-if="v.perms.value.length === 0" icon="shield" title="无需权限" hint="此插件未申请任何权限" />
        <div
          v-for="perm in v.perms.value"
          :key="perm.name"
          class="focusable"
          :class="{ on: focused?.name === perm.name }"
          @mouseenter="focus = perm.name"
          @click="focus = perm.name"
        >
          <PermissionRow :name="perm.name" :granted="perm.granted" :disabled="!v.canEdit.value" :busy="!!v.pending.value[perm.name]" @toggle="v.toggle(perm.name, $event)" />
        </div>
      </section>
      <aside class="explain">
        <template v-if="focused">
          <span class="big-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(focused.name)" /></svg></span>
          <h3 class="ex-title">{{ permissionZh(focused.name) }}</h3>
          <p class="mono muted ex-id">{{ focused.name }}</p>
          <p class="ex-desc">{{ v.permissionDesc(focused.name) }}</p>
          <dl class="kv">
            <div><dt>敏感度</dt><dd><span class="tag" :class="RISK_TONE[v.permissionRisk(focused.name)]">{{ RISK_LABEL[v.permissionRisk(focused.name)] }}</span></dd></div>
            <div><dt>当前</dt><dd>{{ focused.granted ? '已授予' : '未授予' }}</dd></div>
          </dl>
        </template>
        <p v-else class="muted">选择一项权限查看说明</p>
      </aside>
    </div>

    <ContextSlot>
      <MdCard title="批量操作">
        <div class="stack">
          <MdButton variant="tonal" :disabled="!v.canEdit.value || v.summary.value.missing === 0" @click="v.grantAll">全部授予</MdButton>
          <MdButton variant="outlined" :disabled="!v.canEdit.value || v.summary.value.granted === 0" @click="v.revokeAll">全部撤销</MdButton>
          <p v-if="v.disabledHint.value" class="muted small">{{ v.disabledHint.value }}</p>
        </div>
      </MdCard>
    </ContextSlot>
  </div>
</template>

<style scoped>
.cols { display: grid; grid-template-columns: minmax(320px, 1.1fr) minmax(260px, 0.9fr); gap: 20px; align-items: start; }
.focusable { border-radius: var(--radius-m); outline: 2px solid transparent; transition: outline-color var(--dur-short, 0.15s) var(--ease-standard, ease); cursor: default; }
.focusable.on { outline-color: var(--md-primary); }
.explain {
  position: sticky;
  top: 12px;
  background: var(--md-surface-container-low);
  border-radius: var(--radius-l, 18px);
  padding: 20px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}
.big-icon {
  width: 56px;
  height: 56px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-full, 999px);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.big-icon svg { width: 30px; height: 30px; fill: currentColor; }
.ex-title { margin: 6px 0 0; font: 600 18px var(--font-title); color: var(--md-on-surface); }
.ex-id { margin: 0; font-size: 12px; }
.ex-desc { margin: 6px 0 10px; font-size: 14px; line-height: 1.55; color: var(--md-on-surface); }
.small { font-size: 12px; margin: 0; }
</style>
