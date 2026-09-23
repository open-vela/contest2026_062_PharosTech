<script setup lang="ts">
/* Call mini: state line (idle / incoming / active + duration) over a row of
 * contact initial avatars (tap = simulate a call, as CallFeature does) and a
 * red hang-up button while a call is on. Contacts come from
 * nyabula.feature.call (shared with CallFeature); state is the live payload. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

interface Contact { id: string; name: string; note: string }
const mem = useFeatureMemory<{ contacts: Contact[] }>(props.type, {
  contacts: [
    { id: 'c1', name: '妈妈', note: '家人' },
    { id: 'c2', name: '爸爸', note: '家人' },
    { id: 'c3', name: '小雨', note: '朋友' },
    { id: 'c4', name: '王医生', note: '宠物医院' },
  ],
});
const contacts = computed(() => (Array.isArray(mem.contacts) ? mem.contacts : []).slice(0, 6));
const initial = (n: string): string => (n.trim()[0] ?? '?').toUpperCase();

const state = computed<'idle' | 'incoming' | 'active'>(() => {
  const s = props.active ? props.payload?.state : null;
  return s === 'incoming' || s === 'active' ? s : 'idle';
});
const peerName = computed(() => (props.active && typeof props.payload?.name === 'string' ? (props.payload.name as string) : ''));
const muted = computed(() => props.active && props.payload?.muted === true);

const now = ref(Date.now());
let syncAt = Date.now();
let timer: ReturnType<typeof setInterval> | undefined;
watch(() => props.payload, () => { syncAt = Date.now(); now.value = syncAt; });
watch(() => state.value === 'active', (on) => {
  if (timer) clearInterval(timer);
  timer = on ? setInterval(() => { now.value = Date.now(); }, 1000) : undefined;
}, { immediate: true });
onBeforeUnmount(() => { if (timer) clearInterval(timer); });
const durationMs = computed(() => {
  const base = typeof props.payload?.duration_ms === 'number' ? (props.payload.duration_ms as number) : 0;
  return state.value === 'active' ? base + (now.value - syncAt) : base;
});
const pad = (n: number): string => String(n).padStart(2, '0');
const durationText = computed(() => {
  const s = Math.floor(durationMs.value / 1000);
  return `${pad(Math.floor(s / 60))}:${pad(s % 60)}`;
});
const stateText = computed(() => {
  if (state.value === 'incoming') return `${peerName.value} 来电`;
  if (state.value === 'active') return `${peerName.value} · 通话中${muted.value ? ' · 已静音' : ''} ${durationText.value}`;
  return '空闲 · 点头像呼叫';
});

function push(s: 'incoming' | 'active', name: string, duration: number): void {
  void eye.setScene(props.type, eye.sceneStyle, { name, state: s, duration_ms: Math.round(duration), muted: muted.value });
}
function ring(c: Contact): void {
  if (state.value !== 'idle') return;
  push('incoming', c.name, 0);
}
function answer(): void {
  if (state.value === 'incoming') push('active', peerName.value, 0);
}
function hangUp(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="cl">
    <span class="state" :class="state">{{ stateText }}</span>
    <div class="row">
      <div class="avatars">
        <button
          v-for="c in contacts"
          :key="c.id"
          type="button"
          class="av"
          :class="{ on: state !== 'idle' && c.name === peerName }"
          :title="c.name"
          :aria-label="'呼叫 ' + c.name"
          :disabled="state !== 'idle'"
          @click="ring(c)"
        >{{ initial(c.name) }}</button>
      </div>
      <button v-if="state === 'incoming'" type="button" class="rb answer" aria-label="接听" @click="answer"><UiIcon name="call" :size="20" /></button>
      <button v-if="state !== 'idle'" type="button" class="rb hang" aria-label="挂断" @click="hangUp"><UiIcon name="close" :size="20" /></button>
    </div>
  </div>
</template>

<style scoped>
.cl { display: flex; flex-direction: column; gap: 6px; }
.state { font-size: 12.5px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.state.incoming, .state.active { color: var(--md-primary); font-weight: 600; }
.row { display: flex; align-items: center; gap: 8px; }
.avatars { flex: 1; min-width: 0; display: flex; gap: 6px; overflow-x: auto; scrollbar-width: none; }
.avatars::-webkit-scrollbar { display: none; }
.av {
  width: 40px; height: 40px; border-radius: 50%; border: none; flex: none; cursor: pointer;
  background: var(--md-surface-container-highest); color: var(--md-on-surface); font: 700 15px var(--font-body);
}
.av:disabled { cursor: default; opacity: 0.6; }
.av.on { background: var(--md-primary); color: var(--md-on-primary); opacity: 1; }
.rb { width: 40px; height: 40px; border-radius: 50%; border: none; cursor: pointer; display: grid; place-items: center; flex: none; }
.answer { background: var(--md-primary); color: var(--md-on-primary); }
.hang { background: var(--md-error); color: var(--md-on-error); }
</style>
