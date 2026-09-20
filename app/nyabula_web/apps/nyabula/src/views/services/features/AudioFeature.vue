<script setup lang="ts">
/* Playback settings use Core and the actual output device capability query. */
import { computed, ref, watch } from 'vue';
import { MdButton, NkListSection, NkSliderRow, NkStatTile, NkToggleRow } from '@nyabula/ui';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import type { FormFactor } from '../../../composables/useFormFactor';
defineProps<{ type: string; ff: FormFactor }>();
const media = useNativeMedia();
const device = useDeviceRuntime();
const selected = ref('');
const volume = ref(40);
const muted = ref(false);
const outputs = computed(() => device.snapshot?.audioDevices.filter(item => item.available && item.output) ?? []);
watch([() => media.state?.device, () => media.state?.volume, () => media.state?.muted], () => {
  if (!media.state) return;
  selected.value = media.state.device; volume.value = media.state.volume; muted.value = media.state.muted;
}, { immediate: true });
const canVolume = computed(() => media.available && !media.busy && media.state?.volumeSupported === true);
async function setVolume(): Promise<void> { await media.action('volume', { volume: volume.value, muted: muted.value }); }
async function setMuted(value: boolean): Promise<void> { muted.value = value; await setVolume(); }
</script>
<template>
  <div class="feature native-audio" :class="ff">
    <p v-if="!media.available">当前设备未启用原生播放服务。</p>
    <p v-if="device.snapshot?.simulator" class="muted">当前枚举的是模拟器音频设备。</p>
    <p v-if="media.error" class="error" role="alert">{{ media.error }}</p>
    <div class="grid">
      <section class="col">
        <div class="stats">
          <NkStatTile :value="media.state?.volumeSupported ? (media.state.muted ? 0 : media.state.volume) : '—'" unit="%" label="播放音量" icon="volume_up" />
          <NkStatTile :value="media.state?.state === 'playing' ? '播放中' : media.state?.state === 'paused' ? '已暂停' : '空闲'" label="播放器" />
        </div>
        <NkListSection title="已注册输出设备">
          <label class="device-choice">播放输出<select v-model="selected" aria-label="原生音频输出" :disabled="!media.available || media.busy || media.state?.state !== 'idle'"><option v-for="output in outputs" :key="output.path" :value="output.path">{{ output.path }}</option></select></label>
          <p v-if="!outputs.length" class="muted">没有可用输出设备；不会提供虚构的蓝牙或扬声器选项。</p>
          <MdButton :disabled="!selected || !media.available || media.busy || media.state?.state !== 'idle'" @click="media.action('output', { device: selected })">应用播放输出</MdButton>
          <p class="muted">输出切换需先停止播放。这里不改变 MIC 采集路径。</p>
        </NkListSection>
      </section>
      <section class="col">
        <NkListSection title="音量与静音">
          <NkSliderRow v-model="volume" title="播放音量" unit="%" icon-start="volume_down" icon-end="volume_up" :disabled="!canVolume" @commit="setVolume" />
          <NkToggleRow :model-value="muted" icon="volume_off" title="静音" sub="保留设定音量" :disabled="!canVolume" @update:model-value="setMuted" />
          <p v-if="!media.state?.volumeSupported" class="muted">当前驱动未报告音量控制能力。选择支持的输出后才能调整。</p>
          <p v-else class="muted">空闲时保存下次播放音量；播放中应用到设备。设置由 Core 保存。</p>
          <p v-if="media.state && !media.state.settingsSaved" class="error">设备可能已应用，但设置保存失败，请重新读取状态。</p>
          <MdButton variant="text" :disabled="media.busy || !media.available" @click="media.refresh()">重新读取音频状态</MdButton>
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
.stats { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
.device-choice { display: flex; flex-direction: column; gap: 8px; padding: 8px 0; }
select { min-height: 44px; padding: 10px; border-radius: 8px; border: 1px solid var(--md-outline); background: var(--md-surface); color: var(--md-on-surface); font: inherit; }
.error { color: var(--md-error); }
</style>
