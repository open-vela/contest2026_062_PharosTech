<script setup lang="ts">
/* Codec routing and levels come from the audio.* service when the firmware has
 * it; without it (ENOTFOUND) the page keeps the older behaviour: the player's
 * volume through music.volume and the output device node picker. */
import { computed, ref, watch } from 'vue';
import { MdButton, NkChipSelect, NkListSection, NkSliderRow, NkStatTile, NkToggleRow, UiIcon } from '@nyabula/ui';
import { useNativeMedia } from '../../../composables/useNativeMedia';
import { useAudioControl } from '../../../composables/useAudioControl';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';
import type { FormFactor } from '../../../composables/useFormFactor';
import { INPUT_ROUTES, OUTPUT_ROUTES, activeOutputLabel, micGainDb } from '../../../lib/audioControl';
import { useAudioScene } from './sceneLinks';
import EyeShowButton from './EyeShowButton.vue';
const props = defineProps<{ type: string; ff: FormFactor }>();
const media = useNativeMedia();
const audio = useAudioControl();
const device = useDeviceRuntime();
const scene = useAudioScene(props.type, media, audio);
const onEyes = computed(() => scene.shown.value || scene.held.value);
/** The codec service answers and its playback node is there. */
const codec = computed(() => audio.supported === true && audio.status?.available === true);
const out = computed(() => (codec.value ? audio.status?.output ?? null : null));
const mic = computed(() => (codec.value && audio.status?.inputAvailable ? audio.status.input : null));

const selected = ref('');
const volume = ref(40);
const muted = ref(false);
const gain = ref(100);
const advanced = ref(false);
/* A poll must not pull a slider out from under the finger. */
let volumeHeld = false;
let gainHeld = false;
const outputs = computed(() => device.snapshot?.audioDevices.filter(item => item.available && item.output) ?? []);
watch(() => media.state?.device, value => { if (value) selected.value = value; }, { immediate: true });
watch([out, () => media.state?.volume, () => media.state?.muted], () => {
  const level = out.value ?? media.state;
  if (!level) return;
  if (!volumeHeld) volume.value = level.volume;
  muted.value = level.muted;
}, { immediate: true });
watch(mic, value => { if (value && !gainHeld) gain.value = value.gain; }, { immediate: true });

const legacyVolume = computed(() => media.available && !media.busy && media.state?.volumeSupported === true);
const canVolume = computed(() => (codec.value ? audio.canWrite && !audio.busy : legacyVolume.value));
const canCodec = computed(() => audio.canWrite && !audio.busy);
const shownVolume = computed(() => {
  if (out.value) return out.value.muted ? 0 : out.value.volume;
  return media.state?.volumeSupported ? (media.state.muted ? 0 : media.state.volume) : '—';
});

function holdVolume(value: number): void { volumeHeld = true; volume.value = value; }
function holdGain(value: number): void { gainHeld = true; gain.value = value; }
async function setVolume(): Promise<void> {
  try {
    if (codec.value) await audio.set('volume', { volume: volume.value, muted: muted.value });
    else await media.action('volume', { volume: volume.value, muted: muted.value });
  } finally { volumeHeld = false; }
}
async function setMuted(value: boolean): Promise<void> { muted.value = value; await setVolume(); }
async function setGain(): Promise<void> {
  try { await audio.set('mic.gain', { gain: gain.value }); } finally { gainHeld = false; }
}
function setOutputRoute(value: string | string[]): void { if (typeof value === 'string') void audio.set('output.route', { route: value }); }
function setInputRoute(value: string | string[]): void { if (typeof value === 'string') void audio.set('input.route', { route: value }); }
function setChannel(key: 'mono' | 'swap' | 'invertLeft' | 'invertRight', value: boolean): void { void audio.set('channel', { [key]: value }); }
function refresh(): void { void media.refresh(); void audio.refresh(); }
</script>
<template>
  <div class="feature native-audio" :class="ff">
    <p v-if="!media.available && audio.supported !== true">当前设备未启用原生播放服务。</p>
    <p v-if="device.snapshot?.simulator" class="muted">当前枚举的是模拟器音频设备。</p>
    <p v-if="audio.supported === true && audio.status && !audio.status.available" class="muted">音频编解码器节点尚未注册，设备就绪后会自动恢复已保存的设置。</p>
    <p v-if="media.error" class="error" role="alert">{{ media.error }}</p>
    <p v-if="audio.error" class="error" role="alert">{{ audio.error }}</p>
    <div class="grid">
      <section class="col">
        <div class="stats">
          <NkStatTile :value="shownVolume" unit="%" label="播放音量" icon="volume_up" />
          <NkStatTile v-if="out" :value="activeOutputLabel(out)" label="当前输出" />
          <NkStatTile v-else :value="media.state?.state === 'playing' ? '播放中' : media.state?.state === 'paused' ? '已暂停' : '空闲'" label="播放器" />
        </div>
        <NkListSection v-if="out" title="输出路由">
          <NkChipSelect :model-value="out.route" :options="[...OUTPUT_ROUTES]" label="声音从哪里输出" :disabled="!canCodec" @update:model-value="setOutputRoute" />
          <p class="muted">当前实际输出：{{ activeOutputLabel(out) }} · 耳机{{ out.headphones ? '已插入' : '未插入' }}</p>
          <p v-if="out.route === 'auto'" class="muted">自动模式下，插入耳机走耳机，拔出后回到扬声器。</p>
        </NkListSection>
        <NkListSection title="已注册输出设备">
          <label class="device-choice">播放输出<select v-model="selected" aria-label="原生音频输出" :disabled="!media.available || media.busy || media.state?.state !== 'idle'"><option v-for="output in outputs" :key="output.path" :value="output.path">{{ output.path }}</option></select></label>
          <p v-if="!outputs.length" class="muted">没有可用输出设备；不会提供虚构的蓝牙或扬声器选项。</p>
          <MdButton :disabled="!selected || !media.available || media.busy || media.state?.state !== 'idle'" @click="media.action('output', { device: selected })">应用播放输出</MdButton>
          <p class="muted">输出切换需先停止播放。{{ out ? '这里选择的是播放器使用的设备节点，耳机与扬声器在上方“输出路由”切换。' : '这里不改变 MIC 采集路径。' }}</p>
        </NkListSection>
      </section>
      <section class="col">
        <NkListSection title="音量与静音">
          <NkSliderRow :model-value="volume" title="播放音量" unit="%" icon-start="volume_down" icon-end="volume_up" :disabled="!canVolume" @update:model-value="holdVolume" @commit="setVolume" />
          <NkToggleRow :model-value="muted" icon="volume_off" title="静音" sub="保留设定音量" :disabled="!canVolume" @update:model-value="setMuted" />
          <p v-if="out" class="muted">音量直接写入编解码器，播放与否都立即生效。设置由 Core 保存，开机后自动恢复。</p>
          <p v-else-if="!media.state?.volumeSupported" class="muted">当前驱动未报告音量控制能力。选择支持的输出后才能调整。</p>
          <p v-else class="muted">空闲时保存下次播放音量；播放中应用到设备。设置由 Core 保存。</p>
          <p v-if="(out && audio.status && !audio.status.settingsSaved) || (!out && media.state && !media.state.settingsSaved)" class="error">设备可能已应用，但设置保存失败，请重新读取状态。</p>
        </NkListSection>
        <NkListSection v-if="mic" title="麦克风">
          <NkChipSelect :model-value="mic.route" :options="[...INPUT_ROUTES]" label="采集来源" :disabled="!canCodec" @update:model-value="setInputRoute" />
          <NkSliderRow :model-value="gain" title="麦克风增益" unit="%" icon-start="mic" :disabled="!canCodec" @update:model-value="holdGain" @commit="setGain" />
          <p class="muted">编解码器增益为 0–24 dB、每档 3 dB，当前 {{ micGainDb(gain) }} dB。</p>
          <NkToggleRow :model-value="mic.muted" icon="mic" title="麦克风静音" sub="保留采集来源与增益" :disabled="!canCodec" @update:model-value="value => audio.set('mic.mute', { muted: value })" />
        </NkListSection>
        <NkListSection v-if="out" title="高级">
          <button type="button" class="disclosure" :aria-expanded="advanced" @click="advanced = !advanced">
            <UiIcon :name="advanced ? 'expand_less' : 'expand_more'" :size="20" /><span>{{ advanced ? '收起声道处理' : '展开声道处理' }}</span>
          </button>
          <template v-if="advanced">
            <NkToggleRow :model-value="out.mono" icon="tune" title="单声道" sub="左右声道混合后输出" :disabled="!canCodec" @update:model-value="value => setChannel('mono', value)" />
            <NkToggleRow :model-value="out.swap" icon="swap_horiz" title="交换左右声道" :disabled="!canCodec" @update:model-value="value => setChannel('swap', value)" />
            <NkToggleRow :model-value="out.invertLeft" icon="tune" title="左声道反相" :disabled="!canCodec" @update:model-value="value => setChannel('invertLeft', value)" />
            <NkToggleRow :model-value="out.invertRight" icon="tune" title="右声道反相" :disabled="!canCodec" @update:model-value="value => setChannel('invertRight', value)" />
          </template>
        </NkListSection>
        <MdButton variant="text" :disabled="media.busy || audio.busy" @click="refresh">重新读取音频状态</MdButton>
        <EyeShowButton kind="wide" :shown="onEyes" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
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
.disclosure { display: flex; align-items: center; gap: 8px; min-height: 44px; padding: 0; border: 0; background: none; color: var(--md-primary); font: inherit; cursor: pointer; }
.error { color: var(--md-error); }
</style>
