<script setup lang="ts">
/* Task mini: completion ring + the task in focus + show/hide. Tasks are the
 * Core records the full page edits; nothing is kept in the browser. */
import { computed } from 'vue';
import { NkProgressRing } from '@nyabula/ui';
import { useProductRecords } from '../../../composables/useProductRecords';
import { useEyeScene } from '../../../composables/useEyeScene';
import { taskScene } from '../../../composables/eyeScenePayload';
import type { ProductRecord } from '../../../stores/product';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<FeatureMiniProps>();

interface Task extends ProductRecord { id: string; title: string; state: string; progress?: number }
const core = useProductRecords<Task>('task', 5000);
const tasks = computed(() => core.items.value.map((t) => ({ ...t, progress: typeof t.progress === 'number' ? t.progress : t.state === 'done' ? 100 : 0 })));
const total = computed(() => tasks.value.length);
const doneCount = computed(() => tasks.value.filter((t) => t.state === 'done').length);
const ratio = computed(() => (total.value ? doneCount.value / total.value : 0));
/* In focus: what is running, else what waits for the user, else what is next. */
const current = computed(() => tasks.value.find((t) => t.state === 'running') ?? tasks.value.find((t) => t.state === 'confirm')
  ?? tasks.value.find((t) => t.state === 'queued') ?? tasks.value[0] ?? null);
const title = computed(() => current.value?.title ?? (core.available.value ? '暂无待办' : '未连接支持任务的 Core'));
const sub = computed(() => (current.value?.state === 'running' ? `进度 ${Math.round(current.value.progress)}% · ` : '') + `完成 ${doneCount.value} / ${total.value}`);

const scene = useEyeScene(props.type, () => (current.value ? taskScene({ title: current.value.title, progress: current.value.progress, state: current.value.state }) : null), { hideWhenEmpty: true });
</script>

<template>
  <div class="tm">
    <NkProgressRing :value="ratio" :size="44" :stroke="5"><span class="tm-pct">{{ Math.round(ratio * 100) }}%</span></NkProgressRing>
    <div class="tm-text">
      <span class="tm-title">{{ title }}</span>
      <span class="tm-sub">{{ sub }}</span>
    </div>
    <EyeShowButton :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
  </div>
</template>

<style scoped>
.tm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.tm-pct { font: 700 10px var(--font-body); color: var(--md-on-surface); font-variant-numeric: tabular-nums; }
.tm-text { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.tm-title { font: 600 14px var(--font-body); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.tm-sub { font-size: 12px; color: var(--md-on-surface-variant); }
.tm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
