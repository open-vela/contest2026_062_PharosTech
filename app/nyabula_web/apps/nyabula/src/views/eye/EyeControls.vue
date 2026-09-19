<script setup lang="ts">
/* Expression grid, scene groups, style toggle and light preview. Variants
 * decide where it lives (right column / tabs / phone segments). */
import { MdCard, MdChip, MdButton, MdSlider, UiIcon, SegmentedTabs } from '@nyabula/ui';
import { ref } from 'vue';
import type { useEyePage } from './eye.logic';

const props = defineProps<{
  page: ReturnType<typeof useEyePage>;
  /** Which blocks to render (phone segments pass one at a time). */
  only?: 'modes' | 'scenes' | 'light';
  dense?: boolean;
}>();
const { eye, modes, sceneGroups, onLight } = props.page;
const styleItems = [
  { id: 'full', label: '全屏' },
  { id: 'minimal', label: '极简' },
];
const style = ref<'full' | 'minimal'>(eye.sceneStyle);
function pickStyle(v: string) {
  style.value = v as 'full' | 'minimal';
  if (eye.activeScene) void eye.setScene(eye.activeScene, style.value);
  else eye.sceneStyle = style.value;
}
</script>

<template>
  <div class="controls" :class="{ dense }">
    <MdCard v-if="!only || only === 'modes'" title="表情">
      <div class="mode-grid">
        <button
          v-for="m in modes"
          :key="m.id"
          class="mode"
          :class="{ on: eye.activeMode === m.id && !eye.activeScene }"
          :disabled="!page.session.canControl"
          @click="eye.setMode(m.id)"
        >
          <span class="mode-dot" />
          <span>{{ m.label }}</span>
        </button>
      </div>
    </MdCard>

    <MdCard v-if="!only || only === 'scenes'" title="场景">
      <div class="scene-head">
        <SegmentedTabs :items="styleItems" :model-value="style" @update:model-value="pickStyle" />
        <MdButton variant="outlined" :disabled="!eye.activeScene" @click="eye.setScene(null)">退出场景</MdButton>
      </div>
      <div v-for="g in sceneGroups" :key="g.group" class="scene-group">
        <p class="scene-group-title">{{ g.group }}</p>
        <div class="chips">
          <MdChip
            v-for="s in g.scenes"
            :key="s.id"
            :selected="eye.activeScene === s.id"
            :disabled="!page.session.canControl"
            @click="eye.toggleScene(s.id)"
          >
            <UiIcon :name="s.icon" :size="15" /> {{ s.label }}
          </MdChip>
        </div>
      </div>
    </MdCard>

    <MdCard v-if="!only || only === 'light'" title="环境光">
      <div class="light-row">
        <UiIcon name="dark_mode" :size="18" class="muted" />
        <MdSlider :model-value="eye.lightPreview" @update:model-value="onLight" />
        <UiIcon name="light_mode" :size="18" class="muted" />
        <span class="light-val">{{ eye.lightPreview }}%</span>
      </div>
      <p class="light-hint"><span v-if="!eye.ambientOnDevice" class="contract-only">仅本地预览</span> 暗 → 散瞳 · 亮 → 竖瞳{{ eye.ambientOnDevice ? '；设备与预览同步变化' : '' }}</p>
    </MdCard>
  </div>
</template>

<style scoped>
.controls { display: flex; flex-direction: column; gap: 14px; }
.mode-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(96px, 1fr)); gap: 8px; }
.dense .mode-grid { grid-template-columns: repeat(3, 1fr); }
.mode {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px 12px;
  border-radius: var(--radius-m);
  border: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast), transform var(--dur-fast) var(--ease-spring);
}
.mode:hover { border-color: var(--md-outline); color: var(--md-on-surface); }
.mode:active { transform: scale(0.97); }
.mode:disabled { opacity: 0.45; cursor: default; }
.mode.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.mode-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--md-outline); transition: background var(--dur-fast), box-shadow var(--dur-fast); }
.mode.on .mode-dot { background: var(--md-primary); box-shadow: 0 0 8px rgba(var(--md-primary-rgb), 0.7); }
.scene-head { display: flex; align-items: center; justify-content: space-between; gap: 10px; flex-wrap: wrap; margin-bottom: 8px; }
.scene-group { margin-top: 10px; }
.scene-group-title { font-size: 12px; color: var(--md-on-surface-variant); margin: 0 0 6px; letter-spacing: 0.06em; }
.chips { display: flex; flex-wrap: wrap; gap: 8px; }
.chips :deep(.md-chip) { display: inline-flex; align-items: center; gap: 5px; }
.light-row { display: flex; align-items: center; gap: 12px; }
.light-val { font: 600 14px var(--font-body); color: var(--md-primary); min-width: 44px; text-align: right; }
.light-hint { color: var(--md-on-surface-variant); font-size: 12px; margin: 10px 0 0; display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
</style>
