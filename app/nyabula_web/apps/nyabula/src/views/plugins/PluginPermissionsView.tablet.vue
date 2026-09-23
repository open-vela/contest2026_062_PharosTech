<script setup lang="ts">
/* Tablet: single column of cards, each = switch row + inline description. */
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import PermissionRow from './PermissionRow.vue';
import { usePluginPermissionsView } from './PluginPermissionsView.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPermissionsView(props);
const RISK_LABEL = { high: '高敏感', mid: '中等', low: '低' } as const;
const RISK_TONE = { high: 'err', mid: 'warn', low: 'ok' } as const;
</script>

<template>
  <div class="page narrow perms-tablet">
    <div class="row between">
      <div>
        <h2 class="page-title">{{ v.plugin.value?.name ?? v.id.value }} · 权限</h2>
        <p class="page-sub">已授 {{ v.summary.value.granted }}/{{ v.summary.value.total }}</p>
      </div>
      <div class="row">
        <MdButton variant="tonal" :disabled="!v.canEdit.value || v.summary.value.missing === 0" @click="v.grantAll">全部授予</MdButton>
        <MdButton variant="icon" aria-label="打开插件页" @click="v.openPlugin(v.id.value)"><UiIcon name="widgets" :size="20" /></MdButton>
      </div>
    </div>
    <p v-if="v.disabledHint.value" class="muted note"><UiIcon name="lock" :size="14" /> {{ v.disabledHint.value }}</p>

    <div v-if="v.loading.value && !v.plugin.value" class="stack"><Skeleton v-for="i in 4" :key="i" height="120px" radius="var(--radius-l, 18px)" /></div>
    <EmptyState v-else-if="v.errorText.value && !v.plugin.value" tone="error" title="加载失败" :hint="v.errorText.value" action-text="重试" @action="v.retry" />
    <EmptyState v-else-if="v.notFound.value" icon="extension" title="未找到该插件" hint="它可能已被卸载" action-text="返回列表" @action="v.openList" />
    <EmptyState v-else-if="v.perms.value.length === 0" icon="shield" title="无需权限" hint="此插件未申请任何权限" />
    <div v-else class="stack">
      <MdCard v-for="perm in v.perms.value" :key="perm.name">
        <PermissionRow :name="perm.name" :granted="perm.granted" :disabled="!v.canEdit.value" :busy="!!v.pending.value[perm.name]" @toggle="v.toggle(perm.name, $event)" />
        <div class="desc">
          <span class="tag" :class="RISK_TONE[v.permissionRisk(perm.name)]">{{ RISK_LABEL[v.permissionRisk(perm.name)] }}</span>
          <span>{{ v.permissionDesc(perm.name) }}</span>
        </div>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.perms-tablet { display: flex; flex-direction: column; gap: 14px; }
.note { display: inline-flex; align-items: center; gap: 6px; font-size: 13px; margin: 0; }
.desc { display: flex; align-items: flex-start; gap: 10px; margin-top: 10px; font-size: 13.5px; line-height: 1.5; color: var(--md-on-surface-variant); }
.desc .tag { flex: none; }
</style>
