<script setup lang="ts">
/* Nebula eye control (design-lab A): the lens stage with a live caption on
 * the left, a glass control column on the right (expressions / scenes /
 * more with a sliding-light segment). Stacks on tablet portrait and phone.
 * Parity with views/eye (same useEyePage): all expressions, grouped scenes
 * with full/minimal style and exit, ambient light preview, device state
 * readout, gaze drag + toy mode + pairing (NebulaLens), widgets, read-only
 * role handling. */
import { computed, inject, ref } from 'vue';
import { UiIcon } from '@nyabula/ui';
import WidgetGrid from '../../../components/WidgetGrid.vue';
import type { useFormFactor } from '../../../composables/useFormFactor';
import { useEyePage } from '../../../views/eye/eye.logic';
import { MODE_LABELS, SCENE_META } from '../../../stores/eye';
import EyeStatusCard from '../../../views/eye/EyeStatusCard.vue';
import EyeControls from '../../../views/eye/EyeControls.vue';
import NebulaLens from './NebulaLens.vue';

const page = useEyePage();
const { eye, session } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const stacked = computed(() => ff.formFactor.value === 'phone' || (ff.formFactor.value === 'tablet' && ff.orientation.value === 'portrait'));

type Tab = 'modes' | 'scenes' | 'more';
const tab = ref<Tab>('modes');
const TABS: { id: Tab; label: string }[] = [
  { id: 'modes', label: '表情' },
  { id: 'scenes', label: '场景' },
  { id: 'more', label: '更多' },
];
const tabIndex = computed(() => TABS.findIndex((t) => t.id === tab.value));

const caption = computed(() => {
  if (!eye.ready) return session.state === 'pairing-required' ? '等待配对' : session.connected ? '眼睛服务未启动' : '设备离线';
  if (eye.activeScene) return '场景 · ' + (SCENE_META[eye.activeScene]?.label ?? eye.activeScene);
  return '表情 · ' + (MODE_LABELS[eye.activeMode] ?? eye.activeMode);
});

</script>

<template>
  <div class="nb-eye" :class="{ stacked }">
    <section class="stage nb-in">
      <div class="lens-box">
        <NebulaLens
          :ready="page.canvasReady.value"
          :show-pair="page.showPairOverlay.value"
          :pairing="page.pairing.value"
          :pair-error="page.pairError.value"
          @ready="page.onEngineReady"
          @look="page.onLook"
          @pair="page.doPair"
        />
      </div>
      <div class="cap">
        <span class="nb-tag" :class="eye.ready ? 'ok' : ''"><span class="nb-dot" />{{ eye.ready ? '实时同步到双屏' : '未同步' }}</span>
        <Transition name="nb-pop" mode="out-in"><span :key="caption" class="cap-mode nb-title">{{ caption }}</span></Transition>
        <p v-if="!session.canControl && session.connected" class="cap-hint">当前角色只读，无法控制表情与场景</p>
      </div>
    </section>

    <aside class="ctrl nb-glass nb-in" style="--d: 120ms">
      <div class="nb-seg" :style="{ '--n': TABS.length, '--i': tabIndex }">
        <span class="seg-light" />
        <button v-for="t in TABS" :key="t.id" :class="{ on: tab === t.id }" @click="tab = t.id">{{ t.label }}</button>
      </div>

      <EyeControls :page="page" :only="tab === 'more' ? 'light' : tab" /><EyeStatusCard v-if="tab === 'more'" :page="page" />

      <div class="actions">
        <button class="nb-btn primary" :disabled="!session.canControl" @click="eye.setMode(eye.activeMode === 'sleep' ? 'idle' : 'sleep')">
          <UiIcon name="dark_mode" :size="17" /> {{ eye.activeMode === 'sleep' ? '叫醒它' : '哄睡' }}
        </button>
        <button v-if="eye.activeScene" class="nb-btn" @click="eye.setScene(null)"><UiIcon name="close" :size="17" /> 退出场景</button>
      </div>
    </aside>

    <WidgetGrid class="widgets" :form-factor="ff.formFactor.value" />
  </div>
</template>

<style scoped>
.nb-eye {
  display: grid;
  grid-template-columns: minmax(0, 1.35fr) minmax(340px, 1fr);
  gap: 28px;
  align-items: center;
  padding: 0 clamp(16px, 3vw, 36px) 24px;
  max-width: 1320px;
  margin: 0 auto;
  width: 100%;
  min-height: calc(100% - 20px);
}
.stage { display: flex; flex-direction: column; align-items: center; gap: 20px; min-width: 0; }
.lens-box { width: min(600px, 100%); aspect-ratio: 16 / 10; }
.cap { text-align: center; display: flex; flex-direction: column; align-items: center; gap: 8px; }
.cap-mode { font-size: 26px; color: var(--nb-fg); }
.cap-hint { margin: 0; color: var(--nb-dim); font-size: 12.5px; }
.ctrl { padding: 22px; display: flex; flex-direction: column; gap: 18px; min-width: 0; }
.grid { display: grid; gap: 8px; }
.modes { grid-template-columns: repeat(4, 1fr); }
.sc-grid { grid-template-columns: repeat(4, 1fr); }
.cell {
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-sub);
  padding: 12px 4px;
  border-radius: 16px;
  font: 600 13px var(--font-body);
  cursor: pointer;
  animation: nb-rise 0.5s var(--nb-ease) both;
  transition: color 0.2s, background 0.25s, border-color 0.25s, box-shadow 0.3s, transform 0.35s var(--nb-spring);
}
.cell:hover:not(:disabled) { color: var(--nb-fg); background: var(--nb-soft-2); transform: translateY(-2px); }
.cell:active:not(:disabled) { transform: scale(0.93); }
.cell:disabled { opacity: 0.4; cursor: default; }
.cell.on { color: var(--nb-fg); border-color: rgba(var(--nb-acc-rgb), 0.55); background: rgba(var(--nb-acc-rgb), 0.12); box-shadow: 0 0 24px -8px rgba(var(--nb-acc-rgb), 0.8); }
.sc { display: flex; flex-direction: column; align-items: center; gap: 6px; font-size: 12px; }
.sc.on :deep(.ui-icon) { color: var(--nb-acc); }
.scenes { display: flex; flex-direction: column; gap: 14px; max-height: min(52vh, 520px); overflow-y: auto; padding-right: 4px; margin-right: -4px; }
.scene-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; flex-wrap: wrap; }
.nb-seg.mini { width: 150px; }
.nb-seg.mini > button { padding: 6px; font-size: 12.5px; }
.group { display: grid; gap: 8px; }
.more { display: flex; flex-direction: column; gap: 20px; }
.block { display: grid; gap: 10px; }
.light { display: flex; align-items: center; gap: 12px; color: var(--nb-sub); }
.light-val { font: 600 14px var(--font-body); color: var(--nb-acc); min-width: 44px; text-align: right; }
.light-hint { margin: 0; color: var(--nb-sub); font-size: 12px; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.kv { margin: 0; display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
.kv > div { padding: 10px 12px; border-radius: 14px; background: var(--nb-soft); border: 1px solid var(--nb-line); display: flex; flex-direction: column; gap: 2px; }
.kv dt { font-size: 11.5px; color: var(--nb-sub); }
.kv dd { margin: 0; font: 600 13.5px var(--font-body); color: var(--nb-fg); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.empty { margin: 0; color: var(--nb-sub); font-size: 13px; }
.actions { display: flex; gap: 10px; flex-wrap: wrap; }
.widgets { grid-column: 1 / -1; }

.stacked { grid-template-columns: 1fr; gap: 18px; align-items: start; }
.stacked .lens-box { width: 100%; aspect-ratio: auto; height: min(44vh, 400px); }
.stacked .cap-mode { font-size: 22px; }
.stacked .scenes { max-height: none; overflow: visible; }
.stacked .modes, .stacked .sc-grid { grid-template-columns: repeat(3, 1fr); }
.stacked .ctrl { padding: 16px; }
</style>
