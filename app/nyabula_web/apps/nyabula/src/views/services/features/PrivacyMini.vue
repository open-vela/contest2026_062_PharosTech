<script setup lang="ts">
/* Privacy mini: camera / mic / location icon toggles (off = red slashed)
 * + privacy-mode switch. Shares nyabula.feature.privacy with PrivacyFeature;
 * re-pushes { camera, mic, location, privacy_mode } while active. */
import { computed, reactive, watch } from 'vue';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

type Key = 'camera' | 'mic' | 'location';
const ITEMS: { id: Key; icon: string; label: string }[] = [
  { id: 'camera', icon: 'camera', label: '摄像头' },
  { id: 'mic', icon: 'mic', label: '麦克风' },
  { id: 'location', icon: 'language', label: '位置' },
];

const state = reactive(useFeatureMemory(props.type, { camera: true, mic: true, location: false, privacyMode: false }));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const effective = computed(() => ({
  camera: state.camera && !state.privacyMode,
  mic: state.mic && !state.privacyMode,
  location: state.location && !state.privacyMode,
}));
const privacyMode = computed(() => (props.active && typeof props.payload?.privacy_mode === 'boolean' ? props.payload.privacy_mode : state.privacyMode));
function isOn(k: Key): boolean {
  if (props.active && typeof props.payload?.[k] === 'boolean') return props.payload[k] as boolean;
  return effective.value[k];
}

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { ...effective.value, privacy_mode: state.privacyMode });
}
function toggle(k: Key): void {
  if (state.privacyMode) state.privacyMode = false;
  state[k] = !state[k];
  if (props.active) push();
}
function setPrivacy(v: boolean): void {
  state.privacyMode = v;
  if (props.active) push();
}
</script>

<template>
  <div class="pm">
    <div class="pm-icons">
      <button v-for="it in ITEMS" :key="it.id" type="button" class="pm-ic" :class="{ off: !isOn(it.id) }" :title="`${it.label}${isOn(it.id) ? '开启' : '已关闭'}`" :aria-label="it.label" :aria-pressed="isOn(it.id)" @click="toggle(it.id)">
        <UiIcon :name="it.icon" :size="20" />
        <span class="slash" />
      </button>
    </div>
    <label class="pm-sw">
      <UiIcon name="shield" :size="16" :class="{ on: privacyMode }" />
      <span>隐私模式</span>
      <MdSwitch :model-value="privacyMode" @update:model-value="setPrivacy" />
    </label>
  </div>
</template>

<style scoped>
.pm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.pm-icons { flex: 1; display: flex; gap: 4px; }
.pm-ic {
  position: relative; width: 42px; height: 42px; border: none; border-radius: 50%; display: grid; place-items: center; padding: 0; cursor: pointer;
  background: var(--md-secondary-container); color: var(--md-on-secondary-container);
  transition: background var(--dur-fast), color var(--dur-fast);
}
.pm-ic .slash { position: absolute; width: 26px; height: 2px; border-radius: 1px; background: currentColor; transform: rotate(-45deg) scaleX(0); transition: transform var(--dur-fast); }
.pm-ic.off { background: color-mix(in srgb, var(--md-error) 14%, transparent); color: var(--md-error); }
.pm-ic.off .slash { transform: rotate(-45deg) scaleX(1); }
.pm-sw { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--md-on-surface-variant); min-height: 40px; flex: none; cursor: pointer; }
.pm-sw .on { color: var(--md-primary); }
</style>
