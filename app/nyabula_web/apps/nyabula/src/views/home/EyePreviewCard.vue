<script setup lang="ts">
/* Live eye preview (read-only canvas) + current expression/scene + quick
 * expression strip. Click the stage to open the full eye control page. */
import { EyeCanvas, MdCard, MdChip, PairOverlay, UiIcon } from '@nyabula/ui';
import { ref } from 'vue';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage>; height?: string }>();
const { session, eye, modeLabel, sceneLabel, quickModes, go } = props.page;
const pairing = ref(false);
const pairError = ref<string | null>(null);
async function doPair(code: string) {
  pairing.value = true;
  pairError.value = null;
  try {
    await session.pair(code, 'Nyabula Web');
  } catch {
    pairError.value = session.lastError && /wrong pairing code|EPERM/i.test(session.lastError) ? '配对码错误' : session.lastError ?? '配对失败';
  } finally {
    pairing.value = false;
  }
}
</script>

<template>
  <MdCard class="preview">
    <div class="head row between">
      <div class="row" style="gap: 8px">
        <UiIcon name="visibility" :size="18" class="muted" />
        <span class="head-title">眼睛</span>
        <span class="tag">{{ modeLabel }}</span>
        <span v-if="sceneLabel" class="tag info">{{ sceneLabel }}</span>
      </div>
      <button class="link" @click="go('eye')">完整控制 <UiIcon name="chevron_right" :size="16" /></button>
    </div>
    <div class="stage" :style="{ height: height ?? '220px' }" @click="session.connected && go('eye')">
      <EyeCanvas v-if="eye.ready" :eye-state="eye.lastState" :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()" @look="eye.look" />
      <div v-else class="placeholder">
        <UiIcon name="visibility" :size="34" />
        <span>{{ session.connected ? '眼睛服务未启动' : session.state === 'pairing-required' ? '等待配对' : '设备离线' }}</span>
      </div>
      <PairOverlay v-if="session.state === 'pairing-required' || (pairing && session.state === 'authenticating')" :busy="pairing" :error="pairError" @submit="doPair" />
    </div>
    <div class="strip">
      <MdChip
        v-for="m in quickModes"
        :key="m.id"
        :selected="eye.activeMode === m.id && !eye.activeScene"
        :disabled="!session.canControl || !eye.ready"
        @click="eye.setMode(m.id)"
      >{{ m.label }}</MdChip>
      <MdChip v-if="eye.activeScene" @click="eye.setScene(null)"><UiIcon name="close" :size="14" /> 退出场景</MdChip>
    </div>
  </MdCard>
</template>

<style scoped>
.preview { display: flex; flex-direction: column; gap: 12px; }
.head-title { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.link { border: none; background: transparent; color: var(--md-primary); font: 600 13px var(--font-body); cursor: pointer; display: inline-flex; align-items: center; gap: 2px; }
.stage {
  position: relative;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  overflow: hidden;
  cursor: pointer;
}
.stage :deep(.eye-canvas) { width: 100%; height: 100%; border-radius: inherit; pointer-events: none; }
.placeholder { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px; color: var(--md-on-surface-variant); font-size: 13px; }
.strip { display: flex; flex-wrap: wrap; gap: 8px; }
.strip :deep(.md-chip) { display: inline-flex; align-items: center; gap: 4px; }
</style>
