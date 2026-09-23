<script setup lang="ts">
/* Caption feature: enable switch, caption input with a "show" action and a
 * per-browser history list. The sentence is laid out over the device's three
 * caption lines (previous / current / next); the device has no font size. */
import { computed, ref, watch } from 'vue';
import { MdButton, MdTextField, NkListSection, NkRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useEyeScene } from '../../../composables/useEyeScene';
import { captionScene } from '../../../composables/eyeScenePayload';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const HISTORY_MAX = 20;

const mem = useFeatureMemory(props.type, { history: [] as string[] });
const history = ref<string[]>(Array.isArray(mem.history) ? mem.history.slice(0, HISTORY_MAX) : []);
const text = ref('');
/* What this panel last put on the eyes; after a reload it is read back from
 * the caption the device still shows. */
const live = eye.webScene === props.type ? eye.lastState?.scene?.payload as Record<string, unknown> | undefined : undefined;
const lastShown = ref([live?.current_line, live?.next_line].filter((l) => typeof l === 'string' && l).join(''));
const previous = ref(typeof live?.previous_line === 'string' ? live.previous_line : '');
const scene = useEyeScene(props.type, () => (lastShown.value ? captionScene(lastShown.value, previous.value) : null));
const isShown = computed(() => scene.shown.value || scene.held.value);
/* The switch reflects the device state; it is on while the caption scene is shown. */
const enabled = computed({
  get: () => isShown.value,
  set: (v: boolean) => {
    if (!v) void scene.hide();
    else if (lastShown.value) void scene.show();
    else show(text.value || history.value[0] || '');
  },
});
watch(history, () => saveFeatureMemory(props.type, { history: history.value }), { deep: true });

function show(t: string): void {
  const v = t.trim();
  if (!v) return;
  if (v !== lastShown.value) previous.value = lastShown.value;
  lastShown.value = v;
  history.value = [v, ...history.value.filter((h) => h !== v)].slice(0, HISTORY_MAX);
  text.value = '';
  // A new sentence goes out at once, also when the caption is already up.
  void scene.show();
}
function remove(i: number): void {
  history.value = history.value.filter((_, j) => j !== i);
}
function clearHistory(): void {
  history.value = [];
}
</script>

<template>
  <div class="feature" :class="ff">
    <div class="grid">
      <section class="col">
        <NkListSection title="显示">
          <NkToggleRow v-model="enabled" icon="article" title="实时字幕" :sub="isShown ? '正在显示：' + (lastShown || '（空）') : '打开后在设备上显示字幕'" />
        </NkListSection>
        <NkListSection title="字幕内容">
          <div class="compose">
            <MdTextField v-model="text" label="字幕" placeholder="输入要显示的一句话" @keydown.enter.prevent="show(text)" />
            <MdButton class="show" :disabled="!text.trim()" @click="show(text)">显示</MdButton>
          </div>
        </NkListSection>
      </section>
      <section class="col">
        <NkListSection title="历史">
          <template #trailing>
            <MdButton v-if="history.length" variant="text" class="clear" @click="clearHistory">清空</MdButton>
          </template>
          <NkRow v-for="(h, i) in history" :key="h" icon="history" :title="h" :sub="h === lastShown && isShown ? '正在显示' : '点击再次显示'" tappable @tap="show(h)">
            <button type="button" class="del" aria-label="删除" @click.stop="remove(i)"><UiIcon name="close" :size="18" /></button>
          </NkRow>
          <p v-if="!history.length" class="muted empty">还没有显示过字幕</p>
        </NkListSection>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; align-items: start; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.col { display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.compose { display: flex; flex-direction: column; gap: 10px; padding: 8px; }
.show { min-height: 44px; align-self: flex-end; }
.clear { min-height: 44px; }
.del { width: 44px; height: 44px; border: none; background: transparent; color: var(--md-on-surface-variant); border-radius: 50%; display: grid; place-items: center; cursor: pointer; }
.del:hover { background: var(--md-surface-container-highest); }
.empty { margin: 0; padding: 12px; font-size: 13px; text-align: center; }
</style>
