<script setup lang="ts">
/* Live caption feature: enable switch, font size segment, caption text
 * input with a "show" action and a local history list. Showing pushes
 * {text, size} to the device; turning the switch off clears the scene. */
import { computed, ref, watch } from 'vue';
import { MdButton, MdTextField, NkListSection, NkRow, NkSegmentRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const SIZES = [
  { id: 'small', label: '小' },
  { id: 'medium', label: '中' },
  { id: 'large', label: '大' },
];
const HISTORY_MAX = 20;

const mem = useFeatureMemory(props.type, { size: 'medium', history: [] as string[] });
const size = ref(mem.size);
const history = ref<string[]>(Array.isArray(mem.history) ? mem.history.slice(0, HISTORY_MAX) : []);
const text = ref('');
const lastShown = ref('');
const isShown = computed(() => eye.activeScene === props.type);
/* The switch reflects the device state; it is on while the caption scene is shown. */
const enabled = computed({
  get: () => isShown.value,
  set: (v: boolean) => {
    if (v) show(lastShown.value || text.value || ' ');
    else void eye.setScene(null);
  },
});
watch([size, history], () => saveFeatureMemory(props.type, { size: size.value, history: history.value }), { deep: true });
watch(size, () => {
  if (isShown.value) push(lastShown.value);
});

function push(t: string): void {
  void eye.setScene(props.type, eye.sceneStyle, { text: t, size: size.value });
}
function show(t: string): void {
  const v = t.trim();
  if (!v) return;
  lastShown.value = v;
  history.value = [v, ...history.value.filter((h) => h !== v)].slice(0, HISTORY_MAX);
  push(v);
  text.value = '';
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
          <NkSegmentRow v-model="size" title="字号" :items="SIZES" />
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
