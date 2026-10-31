<script setup lang="ts">
/* Instrument eye control, on the shared eye logic (useEyePage). The real
 * EyeStage (canvas, toy mode, pairing overlay, gaze drag) sits in a
 * monitor bezel with live MODE / SCENE / GAZE readouts; every expression
 * is a hardware key; scenes, scene style and the light preview reuse
 * EyeControls; the authoritative eye.state readout goes to the context
 * panel (inline on tablet/phone). Read-only roles get disabled keys. */
import { computed, inject, ref } from 'vue';
import { SegmentedTabs } from '@nyabula/ui';
import ContextSlot from '../../../components/ContextSlot.vue';
import WidgetGrid from '../../../components/WidgetGrid.vue';
import EyeStage from '../../../views/eye/EyeStage.vue';
import IrisCard from '../../../views/eye/IrisCard.vue';
import EyeMirrorCard from '../../../views/eye/EyeMirrorCard.vue';
import EyeControls from '../../../views/eye/EyeControls.vue';
import EyeStatusCard from '../../../views/eye/EyeStatusCard.vue';
import { useEyePage } from '../../../views/eye/eye.logic';
import { SCENE_META } from '../../../stores/eye';
import type { useFormFactor } from '../../../composables/useFormFactor';
import InsPanel from '../parts/InsPanel.vue';
import EyeMonitor from '../parts/EyeMonitor.vue';

const page = useEyePage();
const { eye, session } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const stacked = computed(() => ff.formFactor.value === 'phone' || (ff.formFactor.value === 'tablet' && ff.orientation.value === 'portrait'));
const tab = ref('modes');
const tabs = [
  { id: 'modes', label: '表情', icon: 'face' },
  { id: 'scenes', label: '场景', icon: 'widgets' },
  { id: 'more', label: '更多', icon: 'info' },
];

/* Last gaze point sent from the stage, for the XY readout. */
const gaze = ref<{ x: number; y: number } | null>(null);
function onLook(d: { x?: number; y?: number; release?: boolean }) {
  gaze.value = d.release || typeof d.x !== 'number' || typeof d.y !== 'number' ? null : { x: d.x, y: d.y };
  page.onLook(d);
}
const fmt = (v: number) => (v >= 0 ? '+' : '-') + Math.abs(v).toFixed(2);
const sceneLabel = computed(() => (eye.activeScene ? SCENE_META[eye.activeScene]?.label ?? eye.activeScene : null));
const idx = (i: number) => String(i + 1).padStart(2, '0');
</script>

<template>
  <div class="ins-eye" :class="{ stacked }">
    <section class="stage-col">
      <div class="stage-box">
        <EyeMonitor :live="page.canvasReady.value">
          <EyeStage
            :ready="page.canvasReady.value"
            :show-pair="page.showPairOverlay.value"
            :pairing="page.pairing.value"
            :pair-error="page.pairError.value"
            @ready="page.onEngineReady"
            @look="onLook"
            @pair="page.doPair"
          />
          <template #tl>MODE · {{ eye.activeMode }}<br /><span v-if="sceneLabel">SCENE · {{ sceneLabel }}</span></template>
          <template #bl>GAZE X {{ gaze ? fmt(gaze.x) : '--.--' }} · Y {{ gaze ? fmt(gaze.y) : '--.--' }}</template>
          <template #br>{{ session.canControl ? 'CTRL' : 'READ-ONLY' }}</template>
        </EyeMonitor>
      </div>
      <WidgetGrid v-if="!stacked" :form-factor="ff.formFactor.value" />
    </section>

    <section class="ctl-col">
      <SegmentedTabs v-if="stacked" v-model="tab" :items="tabs" stretch />

      <InsPanel v-if="!stacked || tab === 'modes'" idx="01" code="EXPR" label="表情">
        <template #aside><span class="ro-lbl">{{ page.modes.length }} KEYS</span></template>
        <div class="keypad">
          <button
            v-for="(m, i) in page.modes"
            :key="m.id"
            class="pkey"
            :class="{ on: eye.activeMode === m.id && !eye.activeScene }"
            :disabled="!session.canControl"
            @click="eye.setMode(m.id)"
          >
            <span class="pk-top"><span class="pk-idx">{{ idx(i) }}</span><i class="led" /></span>
            <span class="pk-label">{{ m.label }}</span>
            <span class="pk-id">{{ m.id }}</span>
          </button>
        </div>
      </InsPanel>

      <template v-if="!stacked || tab === 'modes'"><IrisCard /><EyeMirrorCard v-if="!eye.nativeQr" /></template>
      <EyeControls v-if="!stacked || tab === 'scenes'" :page="page" only="scenes" :dense="stacked" />

      <template v-if="!stacked || tab === 'more'">
        <EyeControls :page="page" only="light" />
        <ContextSlot>
          <EyeStatusCard :page="page" />
        </ContextSlot>
        <WidgetGrid v-if="stacked" :form-factor="ff.formFactor.value" />
      </template>
    </section>
  </div>
</template>

<style scoped>
.ins-eye {
  display: grid;
  grid-template-columns: minmax(0, 1.5fr) minmax(340px, 1fr);
  gap: 14px;
  padding: 16px 20px 40px;
  min-height: 100%;
}
.ins-eye.stacked { grid-template-columns: minmax(0, 1fr); padding: 10px 12px 28px; }
.stage-col, .ctl-col { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.stage-box { height: min(64vh, 660px); }
.stacked .stage-box { height: min(50vh, 440px); }
.ro-lbl { font: 600 10px var(--ins-mono); letter-spacing: 0.14em; color: var(--md-outline); }
.keypad { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 6px; }
.stacked .keypad { grid-template-columns: repeat(3, minmax(0, 1fr)); }
.pkey {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 2px;
  padding: 8px 9px 7px;
  min-width: 0;
  border: none;
  border-radius: 3px;
  background: var(--md-surface-container-high);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant), inset 0 -2px 0 var(--ins-key-edge);
  color: var(--md-on-surface-variant);
  text-align: left;
  cursor: pointer;
  transition: box-shadow 0.12s, color 0.12s;
}
.pkey:hover:not(:disabled) { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-outline), inset 0 -2px 0 var(--ins-key-edge); }
.pkey:active:not(:disabled) { transform: translateY(1px); box-shadow: inset 0 0 0 1px var(--md-outline); }
.pkey:disabled { opacity: 0.4; cursor: default; }
.pkey.on { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-primary), inset 0 -2px 0 var(--ins-key-edge); }
.pk-top { display: flex; justify-content: space-between; align-items: center; width: 100%; }
.pk-idx { font: 600 9.5px var(--ins-mono); color: var(--md-outline); letter-spacing: 0.1em; }
.led { width: 6px; height: 6px; border-radius: 50%; background: var(--md-outline-variant); transition: background 0.12s steps(2, jump-none); }
.pkey.on .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.pk-label { font: 600 13px var(--font-body); }
.pk-id { font: 500 9.5px var(--ins-mono); color: var(--md-outline); text-transform: uppercase; letter-spacing: 0.06em; max-width: 100%; overflow: hidden; text-overflow: ellipsis; }
</style>
