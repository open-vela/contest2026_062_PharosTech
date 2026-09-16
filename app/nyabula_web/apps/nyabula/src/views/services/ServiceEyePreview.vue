<script setup lang="ts">
/* Read-only eye preview for the feature detail context panel: live canvas,
 * current expression / feature label and an exit shortcut. */
import { EyeCanvas, MdButton, MdCard, UiIcon } from '@nyabula/ui';
import type { useServiceDetail } from './serviceDetail.logic';

const props = defineProps<{ detail: ReturnType<typeof useServiceDetail>; height?: string }>();
const { session, eye, modeLabel, sceneLabel } = props.detail;
</script>

<template>
  <MdCard class="preview">
    <div class="head">
      <UiIcon name="visibility" :size="18" class="muted" />
      <span class="head-title">眼睛预览</span>
      <span class="tag">{{ modeLabel }}</span>
      <span v-if="sceneLabel" class="tag info">{{ sceneLabel }}</span>
    </div>
    <div class="stage" :style="{ height: height ?? '200px' }">
      <EyeCanvas v-if="eye.ready" :eye-state="eye.lastState" :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()" />
      <div v-else class="placeholder">
        <UiIcon name="visibility" :size="30" />
        <span>{{ session.connected ? '眼睛服务未启动' : session.state === 'pairing-required' ? '等待配对' : '设备离线' }}</span>
      </div>
    </div>
    <MdButton v-if="eye.activeScene" variant="outlined" :disabled="!session.canControl" @click="detail.exit()">退出当前显示</MdButton>
    <p v-else-if="eye.ready" class="muted hint">设备当前处于表情模式</p>
  </MdCard>
</template>

<style scoped>
.preview { display: flex; flex-direction: column; gap: 12px; }
.head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.head-title { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.stage { position: relative; border-radius: var(--radius-m); background: var(--md-surface-container-high); overflow: hidden; }
.stage :deep(.eye-canvas) { width: 100%; height: 100%; border-radius: inherit; pointer-events: none; }
.placeholder { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px; color: var(--md-on-surface-variant); font-size: 13px; }
.hint { font-size: 12.5px; margin: 0; }
</style>
