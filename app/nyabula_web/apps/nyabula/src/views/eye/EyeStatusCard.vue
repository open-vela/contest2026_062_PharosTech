<script setup lang="ts">
/* Authoritative eye.state readout (desktop context panel / tablet detail). */
import { MdCard } from '@nyabula/ui';
import { MODE_LABELS, SCENE_META } from '../../stores/eye';
import type { useEyePage } from './eye.logic';

const props = defineProps<{ page: ReturnType<typeof useEyePage> }>();
const { stateSummary, session } = props.page;
</script>

<template>
  <MdCard title="设备状态">
    <dl v-if="stateSummary" class="kv">
      <div><dt>表情</dt><dd>{{ MODE_LABELS[stateSummary.mode ?? ''] ?? stateSummary.mode }}</dd></div>
      <div><dt>场景</dt><dd>{{ stateSummary.scene ? (SCENE_META[stateSummary.scene]?.label ?? stateSummary.scene) + (stateSummary.style ? ` · ${stateSummary.style}` : '') : '无' }}</dd></div>
      <div><dt>注视</dt><dd>{{ stateSummary.gaze === 'target' ? '跟随目标' : '自动扫视' }}</dd></div>
      <div><dt>环境光</dt><dd>{{ typeof stateSummary.light === 'number' ? Math.round(stateSummary.light * 100) + '%' : '—' }}</dd></div>
      <div><dt>序号</dt><dd class="mono">{{ stateSummary.seq }}</dd></div>
      <div><dt>角色</dt><dd>{{ session.role ?? '—' }}</dd></div>
    </dl>
    <p v-else class="muted" style="font-size: 13px; margin: 0">尚未收到 eye.state</p>
  </MdCard>
</template>
