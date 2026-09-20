<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { MODES, SCENES, type EyeEngine } from '@nyabula/eye-engine';
import { useLinkStore } from '../stores/link';
import EyeCanvas from '../components/EyeCanvas.vue';
import { MdCard, MdChip, MdButton, MdSlider, PairOverlay } from '@nyabula/ui';

const link = useLinkStore();
let engine: EyeEngine | null = null;

const MODE_LABELS: Record<string, string> = {
  idle: '待机', curious: '好奇', happy: '开心', processing: '处理中',
  star: '星星眼', heart: '爱心眼', sleepy: '困倦', sleep: '睡眠',
  angry: '生气', sad: '委屈', surprise: '惊讶', dizzy: '晕', derp: '呆',
};

/* Pairing overlay state: shown over the eye area while the device asks for
 * the on-screen code; the eye canvas only renders once authorized (before
 * that no eye.state arrives, so a placeholder avoids a dead black canvas). */
const pairing = ref(false);
const pairError = ref<string | null>(null);
const connected = computed(() => link.state === 'connected');
const showPairOverlay = computed(
  () => link.state === 'pairing-required' || (pairing.value && link.state === 'authenticating'),
);

async function doPair(code: string) {
  pairing.value = true;
  pairError.value = null;
  try {
    await link.pair(code, 'Nyabula 面板');
  } catch {
    pairError.value =
      link.lastError && /wrong pairing code|EPERM/i.test(link.lastError)
        ? '配对码错误'
        : (link.lastError ?? '配对失败');
  } finally {
    pairing.value = false;
  }
}

/* Single data flow: clicks only send requests; the device's eye.state
 * events drive both the engine (EyeCanvas -> applyRemoteState) and the
 * chip selection below. A click sets a pending (optimistic) value so the
 * chip highlights immediately, then the next eye.state — from any client
 * — becomes the source of truth. */
const pendingMode = ref<string | null>(null);
// undefined = no pending override; null = pending "no scene".
const pendingScene = ref<string | null | undefined>(undefined);
const sceneStyle = ref<'full' | 'minimal'>('full');
const light = ref(55);

const activeMode = computed(
  () => pendingMode.value ?? link.lastEyeState?.expression?.mode ?? 'idle',
);
const activeScene = computed(() =>
  pendingScene.value !== undefined
    ? pendingScene.value
    : (link.lastEyeState?.scene?.type ?? null),
);

watch(
  () => link.lastEyeState,
  (s) => {
    // Remote state supersedes any optimistic click state.
    pendingMode.value = null;
    pendingScene.value = undefined;
    const sc = s?.scene;
    if (sc?.type && (sc.style === 'full' || sc.style === 'minimal')) {
      sceneStyle.value = sc.style;
    }
  },
);

function onEngineReady(e: EyeEngine) {
  engine = e;
}

function pickMode(mode: string) {
  pendingMode.value = mode;
  pendingScene.value = null; // device clears the scene on eye.mode
  void link.send('eye.mode', { mode });
}

function pickScene(type: string) {
  if (activeScene.value === type) {
    exitScene();
    return;
  }
  pendingScene.value = type;
  void link.send('eye.scene', { type, style: sceneStyle.value });
}

function exitScene() {
  pendingScene.value = null;
  void link.send('eye.scene', { type: null });
}

function pickStyle(style: 'full' | 'minimal') {
  sceneStyle.value = style;
  if (activeScene.value) {
    void link.send('eye.scene', { type: activeScene.value, style });
  }
}

function onLight(v: number) {
  light.value = v;
  engine?.setLight(v / 100);
  // Ambient light is device-side state; expose over eye.mode-adjacent topic
  // when core supports it. For now local render only.
}
</script>

<template>
  <div class="home">
    <div class="canvas-panel">
      <div class="eye-area">
        <EyeCanvas v-if="connected" @ready="onEngineReady" />
        <div v-else class="eye-placeholder" />
        <PairOverlay v-if="showPairOverlay" :busy="pairing" :error="pairError" @submit="doPair" />
      </div>
      <p class="hint">点击 / 拖动画布 = 注视点（实时发 eye.look）</p>
    </div>

    <div class="controls">
      <MdCard title="表情">
        <div class="chips">
          <MdChip
            v-for="m in MODES"
            :key="m"
            :selected="activeMode === m && !activeScene"
            @click="pickMode(m)"
          >
            {{ MODE_LABELS[m] ?? m }}
          </MdChip>
        </div>
      </MdCard>

      <MdCard title="场景">
        <div class="chips">
          <MdChip
            v-for="s in SCENES"
            :key="s"
            :selected="activeScene === s"
            @click="pickScene(s)"
          >
            {{ s }}
          </MdChip>
        </div>
        <div class="scene-row">
          <div class="style-toggle">
            <MdChip :selected="sceneStyle === 'full'" @click="pickStyle('full')">全屏</MdChip>
            <MdChip :selected="sceneStyle === 'minimal'" @click="pickStyle('minimal')">极简</MdChip>
          </div>
          <MdButton variant="outlined" :disabled="!activeScene" @click="exitScene">退出场景</MdButton>
        </div>
      </MdCard>

      <MdCard title="环境光">
        <div class="light-row">
          <MdSlider :model-value="light" @update:model-value="onLight" />
          <span class="light-val">{{ light }}%</span>
        </div>
        <p class="light-hint">暗 → 散瞳 · 亮 → 竖瞳</p>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.home {
  display: grid;
  grid-template-columns: minmax(0, 1.4fr) minmax(320px, 1fr);
  gap: 18px;
  padding: 18px;
  height: calc(100% - 58px);
}
.canvas-panel {
  display: flex;
  flex-direction: column;
  min-height: 320px;
}
.canvas-panel > :first-child {
  flex: 1;
  min-height: 0;
}
.eye-area {
  position: relative;
}
.eye-area > .eye-canvas,
.eye-placeholder {
  width: 100%;
  height: 100%;
}
.eye-placeholder {
  border-radius: var(--radius-l);
}
.hint {
  color: var(--md-on-surface-variant);
  font-size: 12px;
  text-align: center;
  margin: 10px 0 0;
}
.controls {
  display: flex;
  flex-direction: column;
  gap: 14px;
  overflow-y: auto;
}
.chips {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.scene-row {
  margin-top: 14px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  gap: 10px;
}
.style-toggle {
  display: flex;
  gap: 8px;
}
.light-row {
  display: flex;
  align-items: center;
  gap: 14px;
}
.light-val {
  font: 600 14px var(--font-body);
  color: var(--md-primary);
  min-width: 44px;
  text-align: right;
}
.light-hint {
  color: var(--md-on-surface-variant);
  font-size: 12px;
  margin: 10px 0 0;
}
@media (max-width: 900px) {
  .home {
    grid-template-columns: 1fr;
  }
}
</style>
