<script setup lang="ts">
/* Desktop: big stage left, controls right column, status + widgets in the
 * shell context panel. */
import { inject } from 'vue';
import ContextSlot from '../../components/ContextSlot.vue';
import WidgetGrid from '../../components/WidgetGrid.vue';
import EyeStage from './EyeStage.vue';
import EyeControls from './EyeControls.vue';
import EyeStatusCard from './EyeStatusCard.vue';
import { useEyePage } from './eye.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const page = useEyePage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
</script>

<template>
  <div class="eye-desktop">
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
      <WidgetGrid :form-factor="ff.formFactor.value" class="widgets" />
    </section>
    <section class="controls-col">
      <EyeControls :page="page" />
    </section>
    <ContextSlot>
      <EyeStatusCard :page="page" />
    </ContextSlot>
  </div>
</template>

<style scoped>
.eye-desktop {
  display: grid;
  grid-template-columns: minmax(0, 1.5fr) minmax(360px, 1fr);
  gap: 20px;
  padding: 20px 24px 40px;
  min-height: 100%;
}
.stage-col { display: flex; flex-direction: column; gap: 16px; min-height: 0; }
.stage-col > :first-child { height: min(62vh, 640px); }
.controls-col { min-width: 0; }
</style>
