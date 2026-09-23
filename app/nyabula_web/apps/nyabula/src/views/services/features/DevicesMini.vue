<script setup lang="ts">
/* Devices mini: how much hardware Core reports as usable (device.status) and
 * a show/hide button. Same source as DevicesFeature. */
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import { hardwareCounts } from '../../../composables/eyeScenePayload';
import { useDevicesScene } from './sceneLinks';
import type { FeatureMiniProps } from './contract';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<FeatureMiniProps>();
const device = useDeviceRuntime();
const scene = useDevicesScene(props.type, device);
const counts = computed(() => hardwareCounts(device.snapshot));
const kinds = computed(() => [
  { icon: 'volume_up', label: '音频', n: counts.value.audio },
  { icon: 'storage', label: '存储', n: counts.value.storage },
  { icon: 'wifi', label: '网络', n: counts.value.network },
]);
</script>

<template>
  <div class="dm">
    <div class="dm-count"><b>{{ device.snapshot ? counts.total : '—' }}</b><span>在线</span></div>
    <div class="dm-icons">
      <span v-for="k in kinds" :key="k.label" class="dm-dev" :class="{ on: k.n > 0 }" :title="`${k.label} ${k.n}`" :aria-label="`${k.label} ${k.n}`">
        <UiIcon :name="k.icon" :size="18" />
      </span>
    </div>
    <EyeShowButton :shown="active || scene.held.value" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
  </div>
</template>

<style scoped>
.dm { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.dm-count { display: flex; align-items: baseline; gap: 4px; flex: none; }
.dm-count b { font: 700 20px var(--font-body); color: var(--md-primary); font-variant-numeric: tabular-nums; }
.dm-count span { font-size: 12px; color: var(--md-on-surface-variant); }
.dm-icons { flex: 1; display: flex; gap: 2px; }
.dm-dev {
  width: 40px; height: 40px; border: none; border-radius: 50%; display: grid; place-items: center; padding: 0;
  background: transparent; color: var(--md-outline);
}
.dm-dev.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.dm-btn { flex: none; min-height: 40px; padding: 0 16px; }
</style>
