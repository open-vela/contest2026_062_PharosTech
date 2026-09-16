<script setup lang="ts">
/* Phone: switch list; tapping the text opens a sheet with the explanation. */
import { ref } from 'vue';
import { BottomSheet, EmptyState, MdButton, Skeleton, UiIcon, permissionIcon, permissionZh } from '@nyabula/ui';
import PermissionRow from './PermissionRow.vue';
import { usePluginPermissionsView } from './PluginPermissionsView.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPermissionsView(props);
const info = ref<string | null>(null);
const RISK_LABEL = { high: '高敏感', mid: '中等', low: '低' } as const;
const RISK_TONE = { high: 'err', mid: 'warn', low: 'ok' } as const;
</script>

<template>
  <div class="perms-phone">
    <div class="summary">
      <span class="sum-main">已授 {{ v.summary.value.granted }}/{{ v.summary.value.total }}</span>
      <span v-if="v.disabledHint.value" class="muted sum-hint"><UiIcon name="lock" :size="13" /> {{ v.disabledHint.value }}</span>
    </div>

    <div v-if="v.loading.value && !v.plugin.value" class="stack"><Skeleton v-for="i in 5" :key="i" height="58px" radius="var(--radius-m)" /></div>
    <EmptyState v-else-if="v.errorText.value && !v.plugin.value" tone="error" title="加载失败" :hint="v.errorText.value" action-text="重试" @action="v.retry" />
    <EmptyState v-else-if="v.notFound.value" icon="extension" title="未找到该插件" hint="它可能已被卸载" action-text="返回列表" @action="v.openList" />
    <EmptyState v-else-if="v.perms.value.length === 0" icon="shield" title="无需权限" hint="此插件未申请任何权限" />
    <div v-else class="stack">
      <div v-for="perm in v.perms.value" :key="perm.name" class="item" @click.self="info = perm.name">
        <PermissionRow :name="perm.name" :granted="perm.granted" :disabled="!v.canEdit.value" :busy="!!v.pending.value[perm.name]" compact @toggle="v.toggle(perm.name, $event)" />
        <button class="info-btn" aria-label="说明" @click="info = perm.name"><UiIcon name="info" :size="18" /></button>
      </div>
    </div>

    <div class="bottom-bar">
      <MdButton variant="tonal" class="wide" :disabled="!v.canEdit.value || v.summary.value.missing === 0" @click="v.grantAll">全部授予</MdButton>
      <MdButton variant="outlined" class="wide" @click="v.openPlugin(v.id.value)">打开插件页</MdButton>
    </div>

    <BottomSheet :open="!!info" :title="info ? permissionZh(info) : ''" @close="info = null">
      <div v-if="info" class="sheet">
        <span class="big-icon"><svg viewBox="0 0 24 24" aria-hidden="true"><path :d="permissionIcon(info)" /></svg></span>
        <p class="mono muted" style="margin: 0; font-size: 12px">{{ info }}</p>
        <p class="desc">{{ v.permissionDesc(info) }}</p>
        <span class="tag" :class="RISK_TONE[v.permissionRisk(info)]">敏感度：{{ RISK_LABEL[v.permissionRisk(info)] }}</span>
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.perms-phone { display: flex; flex-direction: column; gap: 12px; padding: 4px 14px calc(var(--shell-bottom) + 84px); }
.summary { display: flex; flex-direction: column; gap: 4px; }
.sum-main { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.sum-hint { display: inline-flex; align-items: center; gap: 4px; font-size: 12.5px; }
.item { position: relative; }
.item :deep(.perm-row) { padding-right: 44px; }
.info-btn {
  position: absolute;
  right: 60px;
  top: 50%;
  transform: translateY(-50%);
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  padding: 6px;
  border-radius: 50%;
  display: inline-flex;
}
.bottom-bar {
  position: fixed;
  left: 14px;
  right: 14px;
  bottom: calc(var(--shell-bottom) + 12px);
  display: flex;
  gap: 10px;
  z-index: 5;
}
.wide { flex: 1; }
.sheet { display: flex; flex-direction: column; gap: 10px; align-items: flex-start; }
.big-icon {
  width: 52px;
  height: 52px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: 50%;
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.big-icon svg { width: 28px; height: 28px; fill: currentColor; }
.desc { margin: 0; font-size: 14px; line-height: 1.55; color: var(--md-on-surface); }
</style>
