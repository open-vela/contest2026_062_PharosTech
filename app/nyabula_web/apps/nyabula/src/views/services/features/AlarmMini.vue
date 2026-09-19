<script setup lang="ts">
/* Alarm mini: the next alarm as a big time with its enable switch, a "show on
 * the eyes" button and "add". While an alarm rings the card turns into the
 * dismiss / snooze pair. Alarms are Core records (useAlarms). */
import { computed } from 'vue';
import { useRouter } from 'vue-router';
import { MdButton, MdSwitch, UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useAlarms, type Alarm } from '../../../composables/useAlarms';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const { product, sorted, nextAlarm, ringing, scene, dismiss, snooze } = useAlarms();
const session = useSessionStore();
const router = useRouter();

/* Shown row: the next enabled alarm, else the earliest (disabled) one. */
const shown = computed<Alarm | null>(() => nextAlarm.value ?? sorted.value[0] ?? null);
const ring = computed<Alarm | null>(() => ringing.value[0] ?? null);

function toggle(a: Alarm, v: boolean): void {
  void product.mutate('alarm', 'update', { id:a.id, record:{ ...a, enabled:v } });
}
function openAdd(): void {
  if (!session.deviceKey) return;
  void router.push({ name: 'service', params: { key: session.deviceKey, type: props.type } });
}
</script>

<template>
  <div v-if="ring" class="al" role="alert">
    <div class="next">
      <span class="time mono live">{{ ring.time }}</span>
      <span class="label">闹钟到点{{ ring.label ? ' · ' + ring.label : '' }}</span>
    </div>
    <MdButton class="act" :disabled="product.busy.alarm" @click="dismiss(ring)">关闭</MdButton>
    <MdButton variant="tonal" class="act" :disabled="product.busy.alarm" @click="snooze(ring)">稍后</MdButton>
  </div>
  <div v-else class="al">
    <div v-if="shown" class="next" :class="{ off: !shown.enabled }">
      <span class="time mono">{{ shown.time }}</span>
      <span class="label">{{ shown.label ?? '' }}{{ nextAlarm ? '' : ' · 未启用' }}</span>
    </div>
    <span v-else class="empty">还没有闹钟</span>
    <span v-if="shown" class="sw"><MdSwitch :model-value="shown.enabled" @update:model-value="toggle(shown, $event)" /></span>
    <EyeShowButton kind="round" :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
    <button type="button" class="add" aria-label="添加闹钟" @click="openAdd"><UiIcon name="add" :size="18" /><span>添加</span></button>
  </div>
</template>

<style scoped>
.al { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.next { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.time { font-size: 28px; font-weight: 700; line-height: 1; font-variant-numeric: tabular-nums; color: var(--md-on-surface); }
.time.live { color: var(--md-primary); }
.act { flex: none; min-height: 40px; padding: 0 14px; }
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
