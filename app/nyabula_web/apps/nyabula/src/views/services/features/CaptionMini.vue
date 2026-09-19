<script setup lang="ts">
/* Caption mini: single-line input + "show"; while the caption is on the eyes
 * the line the device reports is shown with a hide button. History lives in
 * nyabula.feature.caption (per browser, shared with CaptionFeature). */
import { computed, ref } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeScene } from '../../../composables/useEyeScene';
import { captionScene } from '../../../composables/eyeScenePayload';
import { useSessionStore } from '../../../stores/session';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const session = useSessionStore();

const HISTORY_MAX = 20;
const mem = useFeatureMemory(props.type, { history: [] as string[] });
const text = ref('');
const lastShown = ref('');
const previous = ref('');
const scene = useEyeScene(props.type, () => (lastShown.value ? captionScene(lastShown.value, previous.value) : null));
const line = (key: string): string => (typeof props.payload?.[key] === 'string' ? (props.payload[key] as string) : '');
const liveText = computed(() => (props.active ? (line('current_line') + line('next_line')).trim() : lastShown.value));

function show(): void {
  const v = text.value.trim();
  if (!v) return;
  const history = Array.isArray(mem.history) ? mem.history : [];
  mem.history = [v, ...history.filter((h) => h !== v)].slice(0, HISTORY_MAX);
  saveFeatureMemory(props.type, { history: mem.history });
  previous.value = lastShown.value || liveText.value;
  lastShown.value = v;
  text.value = '';
  void scene.show();
}
</script>

<template>
  <div class="cm">
    <div class="row">
      <input v-model="text" class="in" type="text" placeholder="输入一句话" aria-label="字幕" @keydown.enter.prevent="show" />
      <MdButton class="btn" :disabled="!text.trim() || !session.canControl" @click="show">显示</MdButton>
    </div>
    <div v-if="active || scene.held.value" class="live">
      <UiIcon name="article" :size="16" />
      <span class="live-text">{{ liveText || '（空）' }}</span>
      <button type="button" class="x" aria-label="隐藏字幕" @click="scene.hide()"><UiIcon name="close" :size="16" /></button>
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
