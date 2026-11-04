<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { MdButton, NkColorSwatch, NkSliderRow, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader } from '../featureHeader';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { useLightingStore } from '../../../stores/lighting';
import { useEyeStore } from '../../../stores/eye';
import { LIGHT_ZONES, LIGHT_PRESETS, effectsFor, type LightZone, type LightChange } from '../../../lib/lighting';
import type { FeatureProps } from './contract';
import LightingPreview from './LightingPreview.vue';
defineProps<FeatureProps>();
const lights = useLightingStore();
const eye = useEyeStore();
const selected = ref<LightZone>('left_ear');
const group = computed(() => lights.state?.groups.find(item => item.id === selected.value));
const zone = computed(() => LIGHT_ZONES.find(item => item.id === selected.value)!);
const effects = computed(() => effectsFor(selected.value));
const blocked = computed(() => !lights.canWrite || lights.state?.sleeping === true);
const brightness = ref(0), color = ref('#6fd3ff');
const dragging = ref(false), picking = ref(false);
const colorModes = [{ id: 'fixed', name: '自选颜色' }, { id: 'iris', name: '跟随瞳色' }, { id: 'complement', name: '瞳色互补' }] as const;
watch(() => [selected.value, lights.state?.groups] as const, () => {
  if (!dragging.value) brightness.value = group.value?.brightness ?? 0;
  if (!picking.value) color.value = group.value?.color ?? '#6fd3ff';
}, { immediate: true });
watch(selected, () => { dragging.value = picking.value = false; brightness.value = group.value?.brightness ?? 0; color.value = group.value?.color ?? '#6fd3ff'; });
useVisiblePoll(() => void lights.refresh(), 500);
async function change(patch: LightChange) { await lights.set(selected.value, patch); }
async function commitBrightness(value: number) { await change({ brightness: value }); dragging.value = false; brightness.value = group.value?.brightness ?? 0; }
function inputColor(event: Event) { picking.value = true; color.value = (event.target as HTMLInputElement).value; }
async function commitColor() { await change({ color: color.value }); picking.value = false; color.value = group.value?.color ?? '#6fd3ff'; }
const phase = computed(() => lights.state?.phase === 'boot' ? '开场点亮' : lights.state?.sleeping ? '休眠' : lights.anyOn ? '灯光开启' : '待点亮');
</script>
<template>
 <div class="lighting-feature" :class="ff">
  <FeaturePageHeader icon="light_mode" title="灯光" :subtitle="lights.label" :tone="lights.state?.lastError ? 'error' : 'default'" />
  <div v-if="lights.error || lights.readError" class="lighting-error" role="alert">{{ lights.error || lights.readError }}<button type="button" @click="lights.refresh()">重新读取</button></div>
  <div class="lighting-layout">
   <section class="ui-panel lighting-overview">
    <div class="overview-head"><span class="eyebrow">四区 · 33 颗灯</span><span class="phase-chip">{{ phase }}</span></div>
    <LightingPreview :pixels="lights.state?.pixels" :iris="[eye.lastState?.appearance?.irisLeft ?? '#ffffff', eye.lastState?.appearance?.irisRight ?? '#ffffff']" :selected="selected" />
    <div class="master"><div><strong>{{ lights.label }}</strong></div><MdButton :disabled="!lights.canWrite || (!!lights.state?.sleeping && !lights.anyOn)" @click="lights.set('all', { enabled: !lights.anyOn })">{{ lights.anyOn ? '全部关闭' : '全部打开' }}</MdButton></div>
    <div class="lifecycle-note"><UiIcon name="moon" :size="18"/><p>开机清零并播放短光效，随后归黑；休眠渐暗，唤醒恢复。</p></div>
   </section>
   <section class="ui-panel lighting-controls">
    <div class="zone-tabs" role="tablist" aria-label="灯光区域"><button v-for="item in LIGHT_ZONES" :key="item.id" type="button" role="tab" :aria-selected="selected === item.id" :class="{ selected: selected === item.id }" @click="selected = item.id"><span>{{ item.name }}</span><small>{{ item.count }} 颗</small></button></div>
    <div v-if="!group" class="waiting"><UiIcon name="light_mode" :size="32"/><p>{{ lights.label }}</p></div>
    <template v-else>
     <NkToggleRow :title="`${zone.name}灯光`" :sub="lights.state?.sleeping ? '休眠中，唤醒后恢复' : group.enabled ? '已开启' : '已关闭，可先调整颜色与灯效'" icon="light_mode" :model-value="group.enabled" :disabled="blocked" @update:model-value="value => change({ enabled: value })" />
     <div class="brightness-control"><NkSliderRow :model-value="brightness" title="亮度" :max="lights.state?.maxBrightness ?? 100" unit="%" :disabled="blocked" icon-start="light_mode" @update:model-value="value => { dragging = true; brightness = value; }" @commit="commitBrightness"/><p>调节范围 0– {{ lights.state?.maxBrightness ?? 100 }}%</p></div>
     <div class="color-section"><div class="section-heading"><h3>颜色</h3><code v-if="group.colorMode === 'fixed'">{{ color.toUpperCase() }}</code><span v-else>{{ group.colorMode === 'iris' ? '随对应眼睛同步' : '对应瞳色的互补色' }}</span></div>
      <div v-if="selected === 'left_ear' || selected === 'right_ear'" class="color-modes" aria-label="耳灯颜色来源"><button v-for="mode in colorModes" :key="mode.id" type="button" :aria-pressed="group.colorMode === mode.id" :class="{ on: group.colorMode === mode.id }" :disabled="blocked" @click="change({ colorMode: mode.id })">{{ mode.name }}</button></div>
      <div v-if="group.colorMode === 'fixed'" class="color-pickers"><NkColorSwatch :model-value="color" label="预设灯色" :custom="false" :disabled="blocked" :presets="['#ff7768','#ffbd77','#ffe59b','#6ee7b7','#6fd3ff','#8d8cff','#e69dff','#ffffff']" @update:model-value="value => change({ color: value })"/><label class="custom-color"><input type="color" :value="color" :disabled="blocked" aria-label="自定义灯光颜色" @input="inputColor" @change="commitColor"/><UiIcon name="palette" :size="18"/><span>自定义</span></label></div>
     </div>
     <div class="effect-section"><div class="section-heading"><h3>灯效</h3><span>{{ effects.find(effect => effect.id === group?.effect)?.detail }}</span></div><div class="effect-choices"><button v-for="effect in effects" :key="effect.id" type="button" :aria-pressed="group.effect === effect.id" :class="{ on: group.effect === effect.id }" :disabled="blocked" @click="change({ effect: effect.id })">{{ effect.name }}</button></div></div>
     <div v-if="!['steady','music','status'].includes(group.effect)" class="speed-row"><span>节奏</span><div><button v-for="(label, index) in ['舒缓','自然','轻快']" :key="label" type="button" :class="{ on: group.speed === index + 1 }" :aria-pressed="group.speed === index + 1" :disabled="blocked" @click="change({ speed: index + 1 })">{{ label }}</button></div></div>
     <div v-if="group.effect === 'music'" class="music-feedback"><UiIcon name="music_note" :size="18"/><span>{{ lights.state?.musicActive ? '正在跟随播放电平' : '等待设备播放音乐' }}</span><div class="level-bar" aria-label="音乐电平"><i :style="{ transform: `scaleX(${lights.state?.musicLevel ?? 0})` }"/></div></div>
     <p v-if="group.effect === 'status'" class="state-help">聆听蓝 · 思考紫 · 说话青绿 · 输入异常橙色</p>
    </template>
   </section>
  </div>
  <section class="lighting-presets"><div class="section-heading"><h2>一键氛围</h2><span>同时安排四个区域</span></div><div class="preset-grid"><button v-for="preset in LIGHT_PRESETS" :key="preset.id" type="button" class="ui-panel preset" :class="{ on: lights.state?.preset === preset.id }" :disabled="blocked" :style="{ '--swatch-a': preset.colors[0], '--swatch-b': preset.colors[1] }" @click="lights.preset(preset.id)"><span class="preset-art"><UiIcon :name="preset.icon" :size="23"/></span><strong>{{ preset.name }}</strong><small>{{ preset.detail }}</small></button></div></section>
 </div>
</template>
<style scoped>
.lighting-feature{display:grid;gap:20px;color:var(--md-on-surface)}.lighting-layout{display:grid;grid-template-columns:minmax(240px,.82fr) minmax(360px,1.18fr);gap:18px;align-items:start}.lighting-overview,.lighting-controls{min-width:0;padding:22px}.lighting-overview{display:grid;gap:16px}.overview-head,.section-heading,.master{display:flex;align-items:center;justify-content:space-between;gap:12px}.eyebrow{font-size:12px;font-weight:600;letter-spacing:.1em;color:var(--md-on-surface-variant)}.phase-chip{padding:5px 10px;border-radius:999px;background:var(--md-surface-container-high);color:var(--md-on-surface-variant);font-size:12px;white-space:nowrap}.master{flex-wrap:wrap}.master strong{font-size:16px;font-weight:600}.master p{margin:5px 0 0;color:var(--md-on-surface-variant);font-size:13px}.lifecycle-note{display:flex;align-items:start;gap:9px;padding-top:16px;border-top:1px solid var(--md-outline-variant);color:var(--md-on-surface-variant)}.lifecycle-note>.ui-icon{flex:none;margin-top:2px}.lifecycle-note p{margin:0;font-size:13px;line-height:1.7}.lighting-controls{display:grid;gap:18px}.zone-tabs{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:5px;padding:5px;border-radius:var(--radius-m);background:var(--md-surface-container-low)}.zone-tabs button{display:flex;flex-direction:column;align-items:center;justify-content:center;text-align:center;gap:6px;min-height:72px;padding:10px 8px;border:0;border-radius:var(--radius-s);background:transparent;color:var(--md-on-surface-variant);font:600 16px var(--font-body);line-height:1.25;cursor:pointer}.zone-tabs small{font-size:12px;font-weight:400;line-height:1.2}.zone-tabs .selected{background:var(--md-primary-container);color:var(--md-on-primary-container)}.brightness-control{border-bottom:1px solid var(--md-outline-variant);padding-bottom:16px}.brightness-control p{font-size:12px;color:var(--md-on-surface-variant);margin:0 12px}.section-heading{align-items:baseline;flex-wrap:wrap}.section-heading h2,.section-heading h3{margin:0;font:600 15px var(--font-body)}.section-heading span{color:var(--md-on-surface-variant);font-size:12px;line-height:1.5}.section-heading code{font-size:12px;color:var(--md-on-surface-variant)}.color-section,.effect-section{display:grid;gap:12px}.color-modes,.effect-choices,.speed-row>div{display:flex;gap:7px;flex-wrap:wrap}.color-modes button,.effect-choices button,.speed-row button{min-height:40px;padding:8px 13px;border:1px solid var(--md-outline-variant);border-radius:var(--radius-s);background:transparent;color:var(--md-on-surface-variant);font:500 13px var(--font-body);cursor:pointer}.color-modes .on,.effect-choices .on,.speed-row .on{border-color:var(--md-primary);background:var(--md-secondary-container);color:var(--md-on-secondary-container)}.color-pickers{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.color-pickers :deep(.nk-swatch-list){flex-wrap:wrap}.custom-color{position:relative;display:flex;align-items:center;gap:6px;padding:10px;min-height:40px;border:1px solid var(--md-outline-variant);border-radius:var(--radius-s);font-size:13px;cursor:pointer}.custom-color input{position:absolute;inset:0;opacity:0;width:100%;height:100%;cursor:pointer}.speed-row{display:flex;align-items:center;justify-content:space-between;gap:10px;font-size:13px}.music-feedback{display:flex;align-items:center;gap:8px;color:var(--md-on-surface-variant);font-size:13px}.music-feedback>.ui-icon{color:var(--md-primary)}.level-bar{flex:1;min-width:30px;height:6px;border-radius:4px;overflow:hidden;background:var(--md-surface-container-highest)}.level-bar i{display:block;width:100%;height:100%;background:var(--md-primary);transform-origin:left;transition:transform .15s}.state-help{margin:0;color:var(--md-on-surface-variant);font-size:13px;line-height:1.6}.lighting-presets{display:grid;gap:12px}.preset-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}.preset{border:1px solid var(--md-outline-variant);display:grid;grid-template-columns:44px 1fr;column-gap:12px;row-gap:5px;align-items:center;text-align:left;padding:16px;color:var(--md-on-surface);font:inherit;cursor:pointer}.preset-art{grid-row:span 2;width:44px;height:44px;aspect-ratio:1;align-self:center;display:grid;place-items:center;border-radius:12px;background:linear-gradient(140deg,color-mix(in srgb,var(--swatch-a) 30%,transparent),color-mix(in srgb,var(--swatch-b) 20%,transparent));color:var(--md-on-surface)}.preset strong{font-size:15px}.preset small{font-size:13px;line-height:1.5;color:var(--md-on-surface-variant)}.preset.on{outline:1px solid var(--md-primary);outline-offset:-1px}.waiting{min-height:250px;display:grid;align-content:center;justify-items:center;gap:12px;color:var(--md-on-surface-variant);font-size:14px}.lighting-error{display:flex;gap:12px;align-items:center;padding:12px 16px;color:var(--md-on-error-container);background:var(--md-error-container);border-radius:var(--radius-m);font-size:14px}.lighting-error button{margin-left:auto;flex:none;border:0;background:transparent;color:inherit;font:600 13px var(--font-body);cursor:pointer}button:disabled{opacity:.45;cursor:default}button:focus-visible{outline:2px solid var(--md-primary);outline-offset:3px}
:global([data-skin='nebula'] .lighting-overview){background:radial-gradient(ellipse at 50% 48%,rgba(var(--md-primary-rgb),.09),transparent 65%),var(--nb-glass)}
:global([data-skin='instrument'] .lighting-feature .phase-chip),:global([data-skin='instrument'] .lighting-feature .preset-art){border-radius:2px}
@media(max-width:1050px){.preset-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.lighting-layout{grid-template-columns:minmax(220px,.75fr) minmax(320px,1.25fr)}}
@media(max-width:760px){.lighting-layout{grid-template-columns:1fr}.lighting-overview,.lighting-controls{padding:16px}.lighting-overview :deep(.lighting-preview){max-width:180px}.lighting-feature{gap:16px}.preset{padding:12px;grid-template-columns:34px 1fr;column-gap:8px}.preset-art{width:34px;height:34px}.section-heading span{font-size:13px}.speed-row{flex-wrap:wrap}.zone-tabs button{font-size:15px}}
@media(prefers-reduced-motion:reduce){.level-bar i{transition:none}}
</style>
