<script setup lang="ts">
/* Contract-reserved placeholders: storage / update / logs. The UI shape is
 * final; the device topics are not implemented yet. */
import { computed } from 'vue';
import { EmptyState, MdButton, MdCard, UiIcon } from '@nyabula/ui';

const props = defineProps<{ section?: string }>();

const META: Record<string, { icon: string; title: string; hint: string; actions: { icon: string; label: string }[]; facts: [string, string][] }> = {
  storage: {
    icon: 'storage',
    title: '存储',
    hint: '查看设备存储占用、清理缓存与插件数据。',
    actions: [
      { icon: 'refresh', label: '刷新占用' },
      { icon: 'delete', label: '清理缓存' },
    ],
    facts: [['总容量', '—'], ['已用', '—'], ['插件数据', '—']],
  },
  update: {
    icon: 'download',
    title: '更新',
    hint: '检查 Core 固件更新并在线升级。',
    actions: [
      { icon: 'sync', label: '检查更新' },
      { icon: 'download', label: '立即升级' },
    ],
    facts: [['当前版本', '—'], ['更新通道', '稳定'], ['上次检查', '—']],
  },
  logs: {
    icon: 'terminal',
    title: '日志',
    hint: '实时查看设备日志并导出诊断包。',
    actions: [
      { icon: 'play_arrow', label: '实时日志' },
      { icon: 'upload', label: '导出诊断包' },
    ],
    facts: [['日志级别', '—'], ['缓冲大小', '—']],
  },
};
const meta = computed(() => META[props.section ?? ''] ?? { icon: 'auto_awesome', title: '预留', hint: '此段落尚未定义。', actions: [], facts: [] });
</script>

<template>
  <div class="stack">
    <MdCard>
      <EmptyState :icon="meta.icon" :title="`${meta.title} · 契约预留`" :hint="meta.hint" compact />
      <div class="row" style="justify-content: center; margin-bottom: 8px"><span class="contract-only">契约预留 · 设备端尚未实现对应 NyaLink 主题</span></div>
    </MdCard>
    <MdCard v-if="meta.facts.length" title="概况">
      <dl class="kv">
        <div v-for="[k, v] in meta.facts" :key="k"><dt>{{ k }}</dt><dd class="muted">{{ v }}</dd></div>
      </dl>
    </MdCard>
    <MdCard v-if="meta.actions.length" title="操作">
      <div class="row wrap">
        <MdButton v-for="a in meta.actions" :key="a.label" variant="tonal" disabled><UiIcon :name="a.icon" :size="16" /> {{ a.label }}</MdButton>
      </div>
    </MdCard>
  </div>
</template>

<style scoped>
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
