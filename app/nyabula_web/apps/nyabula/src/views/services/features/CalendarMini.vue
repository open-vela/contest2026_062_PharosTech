<script setup lang="ts">
/* Calendar mini: today's date, the nearest upcoming event with its
 * countdown, and a show/hide button. Events are read from
 * nyabula.feature.calendar (shared with CalendarFeature); "show" pushes the
 * same countdown payload the full page does. */
import { computed } from 'vue';
import { MdButton } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

interface CalEvent { id: string; title: string; date: string }
const iso = (d: Date): string => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
const today = new Date();
const todayIso = iso(today);
const WEEK = ['日', '一', '二', '三', '四', '五', '六'];
const todayText = `${today.getMonth() + 1} 月 ${today.getDate()} 日 周${WEEK[today.getDay()]}`;

const mem = useFeatureMemory<{ events: CalEvent[] }>(props.type, {
  events: [{ id: 'e1', title: '小猫生日', date: iso(new Date(today.getFullYear(), today.getMonth() + 1, 15)) }],
});
const daysLeft = (date: string): number => Math.round((new Date(date + 'T00:00:00').getTime() - new Date(todayIso + 'T00:00:00').getTime()) / 86400000);
const upcoming = computed(() => (Array.isArray(mem.events) ? mem.events : []).filter((e) => daysLeft(e.date) >= 0).sort((a, b) => a.date.localeCompare(b.date)));
const countdownText = (n: number): string => (n === 0 ? '就是今天' : `还有 ${n} 天`);
const line = computed(() => {
  if (props.active && typeof props.payload?.title === 'string') {
    const n = typeof props.payload.days_left === 'number' ? (props.payload.days_left as number) : null;
    return `${props.payload.title}${n === null ? '' : ' · ' + countdownText(n)}`;
  }
  const n = upcoming.value[0];
  return n ? `${n.title} · ${countdownText(daysLeft(n.date))}` : '暂无即将到来的日程';
});

function show(): void {
  const n = upcoming.value[0];
  if (!n) return;
  void eye.setScene(props.type, eye.sceneStyle, {
    title: n.title,
    date: n.date,
    days_left: daysLeft(n.date),
    events: upcoming.value.slice(0, 5).map((e) => ({ title: e.title, date: e.date, days_left: daysLeft(e.date) })),
  });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="cal">
    <div class="info">
      <span class="today">{{ todayText }}</span>
      <span class="ev" :class="{ live: active }">{{ line }}</span>
    </div>
    <MdButton v-if="active" variant="outlined" class="btn" @click="hide">隐藏</MdButton>
    <MdButton v-else variant="tonal" class="btn" :disabled="!upcoming.length" @click="show">显示</MdButton>
  </div>
</template>

<style scoped>
.cal { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.info { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.today { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.ev { font-size: 12.5px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.ev.live { color: var(--md-primary); }
.btn { min-height: 40px; padding: 0 18px; flex: none; }
</style>
