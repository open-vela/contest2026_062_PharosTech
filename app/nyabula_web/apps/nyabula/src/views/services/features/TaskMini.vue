<script setup lang="ts">
/* Task mini: completion ring + current task name + "push". Shares
 * nyabula.feature.task (tasks + selection) with TaskFeature. */
import { computed, reactive, watch } from 'vue';
import { MdButton, NkProgressRing } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

type TaskState = 'queued' | 'running' | 'done' | 'failed';
interface Task { id: string; title: string; done: boolean; state: TaskState; progress: number }
const state = reactive(useFeatureMemory(props.type, {
  tasks: [
    { id: 't1', title: '整理今天的照片', done: false, state: 'running', progress: 40 },
    { id: 't2', title: '提醒晚上喂猫', done: true, state: 'done', progress: 100 },
    { id: 't3', title: '下载新的语音包', done: false, state: 'queued', progress: 0 },
  ] as Task[],
  selected: 't1',
}));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const total = computed(() => state.tasks.length);
const doneCount = computed(() => state.tasks.filter((t) => t.done).length);
const ratio = computed(() => (total.value ? doneCount.value / total.value : 0));
const current = computed<Task | null>(() => state.tasks.find((t) => t.id === state.selected) ?? state.tasks[0] ?? null);
const title = computed(() => (props.active && typeof props.payload?.title === 'string' ? props.payload.title : current.value?.title ?? '暂无待办'));
const liveProgress = computed(() => (props.active && typeof props.payload?.progress === 'number' ? Math.round(props.payload.progress * 100) : null));
const sub = computed(() => (liveProgress.value !== null ? `进度 ${liveProgress.value}%` : `完成 ${doneCount.value} / ${total.value}`));

function push(): void {
  const t = current.value;
  if (!t) return;
  void eye.setScene(props.type, eye.sceneStyle, { title: t.title, progress: t.progress / 100, state: t.state });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="tm">
    <NkProgressRing :value="ratio" :size="44" :stroke="5"><span class="tm-pct">{{ Math.round(ratio * 100) }}%</span></NkProgressRing>
    <div class="tm-text">
      <span class="tm-title">{{ title }}</span>
      <span class="tm-sub">{{ sub }}</span>
    </div>
    <MdButton v-if="!active" variant="tonal" class="tm-btn" :disabled="!current" @click="push">推送</MdButton>
    <MdButton v-else variant="text" class="tm-btn" @click="hide">隐藏</MdButton>
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
