<script setup lang="ts">
/* Two-point calibration of the ambient light sensor.  A photodiode over a
 * fixed resistor is monotonic and does not drift, so one reading with the
 * sensor covered and one in the brightest light it will meet fix both the
 * range and the direction -- which learning from daily use can only guess. */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { MdCard, MdButton } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';

const describeError = (cause: unknown): string => cause instanceof Error ? cause.message : String(cause);

interface LightStatus {
  available: boolean; enabled: boolean; raw: number | null; smoothed: number | null;
  min: number | null; max: number | null; level: number | null; dilation: number | null;
  polarity: string; resolved: string; pinned: boolean;
}
/** Readings closer than this are one reading, not two ends of a range. */
const MIN_SPAN = 24;

const session = useSessionStore();
const status = ref<LightStatus | null>(null);
const dark = ref<number | null>(null);
const bright = ref<number | null>(null);
const error = ref('');
const busy = ref(false);
let timer: number | undefined;

async function refresh(): Promise<void> {
  if (!session.connected || document.hidden) return;
  try { status.value = await session.request('light.status') as unknown as LightStatus; error.value = ''; }
  catch (e) { error.value = describeError(e); }
}
onMounted(() => { void refresh(); timer = window.setInterval(() => void refresh(), 1000); });
onBeforeUnmount(() => window.clearInterval(timer));

const reading = computed(() => status.value?.smoothed ?? status.value?.raw ?? null);
const span = computed(() => dark.value !== null && bright.value !== null ? Math.abs(dark.value - bright.value) : null);
const ready = computed(() => span.value !== null && span.value >= MIN_SPAN);
const pct = (v: number | null | undefined) => v === null || v === undefined ? '—' : `${Math.round(v * 100)}%`;

async function configure(data: Record<string, unknown>): Promise<void> {
  busy.value = true;
  try { status.value = await session.request('light.config', data) as unknown as LightStatus; error.value = ''; }
  catch (e) { error.value = describeError(e); }
  finally { busy.value = false; }
}
async function apply(): Promise<void> {
  if (!ready.value || dark.value === null || bright.value === null) return;
  await configure({
    polarity: bright.value < dark.value ? 'bright-low' : 'bright-high',
    min: Math.round(Math.min(dark.value, bright.value)),
    max: Math.round(Math.max(dark.value, bright.value)),
  });
  if (!error.value) { dark.value = null; bright.value = null; }
}
</script>

<template>
  <MdCard title="环境光传感器校准">
    <p v-if="status && !status.available" class="hint">设备没有报告环境光传感器。</p>
    <template v-else>
      <div class="now">
        <div><span class="k">当前读数</span><strong>{{ reading === null ? '—' : Math.round(reading) }}</strong></div>
        <div><span class="k">亮度</span><strong>{{ pct(status?.level) }}</strong></div>
        <div><span class="k">散瞳</span><strong>{{ pct(status?.dilation) }}</strong></div>
        <div><span class="k">量程</span><strong>{{ status?.min == null ? '—' : `${Math.round(status.min)}–${Math.round(status.max ?? 0)}` }}</strong><small>{{ status?.pinned ? '已校准' : '学习中' }}</small></div>
      </div>
      <ol class="steps">
        <li>
          <span>用手完全遮住传感器</span>
          <MdButton variant="tonal" :disabled="reading === null || !session.isOwner" @click="dark = reading">记录暗点{{ dark === null ? '' : ` · ${Math.round(dark)}` }}</MdButton>
        </li>
        <li>
          <span>放到会遇到的最亮环境（或用手机手电照一下）</span>
          <MdButton variant="tonal" :disabled="reading === null || !session.isOwner" @click="bright = reading">记录亮点{{ bright === null ? '' : ` · ${Math.round(bright)}` }}</MdButton>
        </li>
      </ol>
      <p v-if="span !== null && !ready" class="warn" role="alert">两次读数只差 {{ Math.round(span) }}，不足以作为量程。请确认遮严了、照亮了再各记一次。</p>
      <div class="actions">
        <MdButton variant="filled" :disabled="!ready || busy || !session.isOwner" @click="apply">应用校准</MdButton>
        <MdButton variant="text" :disabled="busy || !status?.pinned || !session.isOwner" @click="configure({ pinned: false, polarity: 'auto' })">改回自动学习</MdButton>
      </div>
      <p v-if="error" class="warn" role="alert">{{ error }}</p>
      <p class="hint">方向（越亮读数越高还是越低）由两点自动判断。校准后量程固定，不再随日常使用漂移。</p>
    </template>
  </MdCard>
</template>

<style scoped>
.now { display: grid; grid-template-columns: repeat(auto-fit, minmax(92px, 1fr)); gap: 10px; margin-bottom: 14px; }
.now > div { display: flex; flex-direction: column; gap: 2px; }
.k { font-size: 12px; color: var(--md-on-surface-variant); }
.now strong { font: 600 18px var(--font-title); font-variant-numeric: tabular-nums; }
.now small { font-size: 11.5px; color: var(--md-on-surface-variant); }
.steps { margin: 0; padding-left: 1.3em; display: flex; flex-direction: column; gap: 10px; }
.steps li > span { display: block; margin-bottom: 6px; font-size: 13.5px; line-height: 1.5; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 14px; }
.hint { margin: 12px 0 0; font-size: 12px; line-height: 1.5; color: var(--md-on-surface-variant); }
.warn { margin: 10px 0 0; font-size: 12.5px; line-height: 1.5; color: var(--md-error); }
</style>
