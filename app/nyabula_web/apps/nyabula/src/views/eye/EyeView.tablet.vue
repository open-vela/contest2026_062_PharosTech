<script setup lang="ts">
/* Tablet: landscape = stage + scrollable control column; portrait = stage on
 * top, segmented tabs (表情 / 场景 / 状态) below. */
import { computed, inject, ref } from 'vue';
import { SegmentedTabs } from '@nyabula/ui';
import WidgetGrid from '../../components/WidgetGrid.vue';
import EyeStage from './EyeStage.vue';
import EyeControls from './EyeControls.vue';
import EyeStatusCard from './EyeStatusCard.vue';
import { useEyePage } from './eye.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const page = useEyePage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
const tab = ref('modes');
const tabs = [
  { id: 'modes', label: '表情', icon: 'face' },
  { id: 'scenes', label: '场景', icon: 'widgets' },
  { id: 'status', label: '状态', icon: 'info' },
];
</script>

<template>
  <div class="eye-tablet" :class="{ portrait }">
    <section class="stage-col">
      <EyeStage
        :ready="page.canvasReady.value"
        :show-pair="page.showPairOverlay.value"
        :pairing="page.pairing.value"
        :pair-error="page.pairError.value"
        @ready="page.onEngineReady"
        @look="page.onLook"
        @pair="page.doPair"
      />
    </section>
    <section v-if="!portrait" class="side">
      <EyeControls :page="page" />
      <EyeStatusCard :page="page" />
      <WidgetGrid :form-factor="ff.formFactor.value" />
    </section>
    <section v-else class="below">
      <SegmentedTabs v-model="tab" :items="tabs" stretch />
      <EyeControls v-if="tab === 'modes'" :page="page" only="modes" />
      <EyeControls v-else-if="tab === 'scenes'" :page="page" only="scenes" />
      <template v-else>
        <EyeStatusCard :page="page" />
        <EyeControls :page="page" only="light" />
        <WidgetGrid :form-factor="ff.formFactor.value" />
      </template>
    </section>
  </div>
</template>

<style scoped>
.eye-tablet {
  display: grid;
  grid-template-columns: minmax(0, 1.2fr) minmax(320px, 1fr);
  gap: 18px;
  padding: 18px 20px 40px;
  min-height: 100%;
}
.eye-tablet.portrait { grid-template-columns: 1fr; }
.stage-col { --eye-stage-max-height: min(60vh, 560px); }
.portrait .stage-col { --eye-stage-max-height: min(46vh, 480px); }
.side { display: flex; flex-direction: column; gap: 14px; }
.below { display: flex; flex-direction: column; gap: 14px; }
</style>
