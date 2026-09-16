<script setup lang="ts">
/* Caption mini: single-line input + "show" button; while active the text
 * currently on the device is shown with a hide button. History and font
 * size live in nyabula.feature.caption (shared with CaptionFeature). */
import { computed, ref } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

const HISTORY_MAX = 20;
const mem = useFeatureMemory(props.type, { size: 'medium', history: [] as string[] });
const text = ref('');
const liveText = computed(() => (props.active && typeof props.payload?.text === 'string' ? (props.payload.text as string).trim() : ''));

function show(): void {
  const v = text.value.trim();
  if (!v) return;
  const history = Array.isArray(mem.history) ? mem.history : [];
  mem.history = [v, ...history.filter((h) => h !== v)].slice(0, HISTORY_MAX);
  saveFeatureMemory(props.type, { size: mem.size, history: mem.history });
  void eye.setScene(props.type, eye.sceneStyle, { text: v, size: mem.size });
  text.value = '';
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="cm">
    <div class="row">
      <input v-model="text" class="in" type="text" placeholder="输入一句话" aria-label="字幕" @keydown.enter.prevent="show" />
      <MdButton class="btn" :disabled="!text.trim()" @click="show">显示</MdButton>
    </div>
    <div v-if="active" class="live">
      <UiIcon name="article" :size="16" />
      <span class="live-text">{{ liveText || '（空）' }}</span>
      <button type="button" class="x" aria-label="隐藏字幕" @click="hide"><UiIcon name="close" :size="16" /></button>
    </div>
  </div>
</template>

<style scoped>
.cm { display: flex; flex-direction: column; gap: 6px; }
.row { display: flex; gap: 8px; align-items: center; }
.in {
  flex: 1; min-width: 0; height: 40px; padding: 0 12px;
  border: 1px solid var(--md-outline-variant); border-radius: var(--radius-m);
  background: var(--md-surface-container-lowest, var(--md-surface)); color: var(--md-on-surface);
  font: 14px var(--font-body); outline: none;
}
.in:focus { border-color: var(--md-primary); }
.btn { min-height: 40px; padding: 0 18px; flex: none; }
.live { display: flex; align-items: center; gap: 6px; min-height: 28px; color: var(--md-primary); font-size: 12.5px; }
.live-text { flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.x { width: 28px; height: 28px; border: none; border-radius: 50%; background: transparent; color: var(--md-on-surface-variant); cursor: pointer; display: grid; place-items: center; }
.x:hover { background: var(--md-surface-container-highest); }
</style>
