<script setup lang="ts">
/* Calendar mini: today's date, the nearest upcoming event with its countdown
 * and a show/hide button. Events are the Core calendar records the full page
 * edits; "show" pushes the same scene the full page does. */
import { computed } from 'vue';
import { useProductRecords } from '../../../composables/useProductRecords';
import { useEyeScene } from '../../../composables/useEyeScene';
import { calendarCountdown, calendarScene } from '../../../composables/eyeScenePayload';
import type { ProductRecord } from '../../../stores/product';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();

interface CalRecord extends ProductRecord { id: string; title: string; start_at: number }
const core = useProductRecords<CalRecord>('calendar', 5000);
const today = new Date();
const WEEK = ['日', '一', '二', '三', '四', '五', '六'];
const todayText = `${today.getMonth() + 1} 月 ${today.getDate()} 日 周${WEEK[today.getDay()]}`;
const startOfToday = new Date(today.getFullYear(), today.getMonth(), today.getDate()).getTime();

const next = computed<CalRecord | null>(() => core.items.value.filter((e) => e.start_at >= startOfToday)
  .sort((a, b) => a.start_at - b.start_at)[0] ?? null);
const line = computed(() => !core.available.value ? '未连接支持日历的 Core'
  : next.value ? `${next.value.title} · ${calendarCountdown(next.value.start_at)}` : '暂无即将到来的日程');

const scene = useEyeScene(props.type, () => (next.value ? calendarScene({ title: next.value.title, startAt: next.value.start_at }) : null), { hideWhenEmpty: true });
</script>

<template>
  <div class="cal">
    <div class="info">
      <span class="today">{{ todayText }}</span>
      <span class="ev" :class="{ live: active }">{{ line }}</span>
    </div>
    <EyeShowButton :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
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
