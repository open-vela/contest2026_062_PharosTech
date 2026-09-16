<script setup lang="ts">
/* Pairing mini: "show pairing code" / "hide" toggle + one-line note. The
 * code itself is only rendered on the device screen, never here (same as
 * PairingFeature, which pushes the scene in 'full' style). */
import { MdButton, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import type { FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

function show(): void {
  void eye.setScene(props.type, 'full');
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="pam">
    <span class="pam-note">
      <UiIcon :name="active ? 'qr_code' : 'lock'" :size="16" :class="{ on: active }" />
      {{ active ? '配对码正在眼睛上显示' : '配对码只在设备屏幕显示' }}
    </span>
    <MdButton v-if="!active" variant="tonal" class="pam-btn" @click="show"><UiIcon name="qr_code" :size="16" /> 显示配对码</MdButton>
    <MdButton v-else variant="outlined" class="pam-btn" @click="hide">隐藏</MdButton>
  </div>
</template>

<style scoped>
.pam { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.pam-note { flex: 1; min-width: 0; display: inline-flex; align-items: center; gap: 6px; font-size: 12.5px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.pam-note .on { color: var(--md-primary); }
.pam-btn { flex: none; min-height: 40px; padding: 0 16px; display: inline-flex; align-items: center; gap: 4px; }
</style>
