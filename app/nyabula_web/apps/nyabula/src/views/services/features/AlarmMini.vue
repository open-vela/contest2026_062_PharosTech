<script setup lang="ts">
/* Alarm mini: next enabled alarm as a big time with its enable switch, and
 * an "add" button that opens the full page. Alarms live in
 * nyabula.feature.alarm (shared with AlarmFeature); toggling re-pushes the
 * next alarm the same way the full page does (none enabled -> hide). */
import { computed } from 'vue';
import { useRouter } from 'vue-router';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useAlarms, type Alarm } from '../../../composables/useAlarms';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const { product, sorted, nextAlarm } = useAlarms();
const session = useSessionStore();
const router = useRouter();

/* Shown row: the next enabled alarm, else the earliest (disabled) one. */
const shown = computed<Alarm | null>(() => nextAlarm.value ?? sorted.value[0] ?? null);
const liveTime = computed(() => (props.active && typeof props.payload?.time === 'string' ? (props.payload.time as string) : null));
const liveLabel = computed(() => (props.active && typeof props.payload?.label === 'string' ? (props.payload.label as string) : null));

function toggle(a: Alarm, v: boolean): void {
  void product.mutate('alarm', 'update', { id:a.id, record:{ ...a, enabled:v } });
}
function openAdd(): void {
  if (!session.deviceKey) return;
  void router.push({ name: 'service', params: { key: session.deviceKey, type: props.type } });
}
</script>

<template>
  <div class="al">
    <div v-if="shown" class="next" :class="{ off: !shown.enabled }">
      <span class="time mono">{{ liveTime ?? shown.time }}</span>
      <span class="label">{{ liveLabel ?? shown.label ?? '' }}{{ nextAlarm ? '' : ' · 未启用' }}</span>
    </div>
    <span v-else class="empty">还没有闹钟</span>
    <span v-if="shown" class="sw"><MdSwitch :model-value="shown.enabled" @update:model-value="toggle(shown, $event)" /></span>
    <button type="button" class="add" aria-label="添加闹钟" @click="openAdd"><UiIcon name="add" :size="18" /><span>添加</span></button>
  </div>
</template>

<style scoped>
.al { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.next { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.time { font-size: 28px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; color: var(--md-on-surface); }
.next.off .time { color: var(--md-on-surface-variant); }
.label { font-size: 12px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.empty { flex: 1; font-size: 13px; color: var(--md-on-surface-variant); }
.sw { display: inline-flex; align-items: center; min-height: 40px; flex: none; }
.add {
  display: inline-flex; align-items: center; gap: 2px; min-height: 40px; padding: 0 12px 0 8px; flex: none;
  border: none; border-radius: var(--radius-full); cursor: pointer;
  background: var(--md-secondary-container); color: var(--md-on-secondary-container); font: 600 13px var(--font-body);
}
</style>
