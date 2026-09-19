<script setup lang="ts">
/* Phone: the two eyes float at the top, quick expression strip beneath,
 * segmented sections (表情 / 场景 / 更多) below; scene picking opens a sheet
 * with grouped list for one-hand reach. */
import { inject, ref } from 'vue';
import { BottomSheet, MdButton, SegmentedTabs, UiIcon } from '@nyabula/ui';
import WidgetGrid from '../../components/WidgetGrid.vue';
import EyeStage from './EyeStage.vue';
import EyeControls from './EyeControls.vue';
import EyeStatusCard from './EyeStatusCard.vue';
import { useEyePage } from './eye.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const page = useEyePage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const tab = ref('modes');
const tabs = [
  { id: 'modes', label: '表情' },
  { id: 'scenes', label: '场景' },
  { id: 'more', label: '更多' },
];
const sceneSheet = ref(false);
const { eye } = page;
</script>

<template>
  <div class="eye-phone">
    <div class="stage-wrap">
      <EyeStage
        :ready="page.canvasReady.value"
        :show-pair="page.showPairOverlay.value"
        :pairing="page.pairing.value"
        :pair-error="page.pairError.value"
        @ready="page.onEngineReady"
        @look="page.onLook"
        @pair="page.doPair"
      />
    </div>

    <div class="quick-strip">
      <button
        v-for="m in page.modes.slice(0, 6)"
        :key="m.id"
        class="quick"
        :class="{ on: eye.activeMode === m.id && !eye.activeScene }"
        :disabled="!page.session.canControl"
        @click="eye.setMode(m.id)"
      >
        {{ m.label }}
      </button>
      <button class="quick more" @click="sceneSheet = true"><UiIcon name="widgets" :size="16" /> 场景</button>
    </div>

    <div class="body">
      <SegmentedTabs v-model="tab" :items="tabs" stretch />
      <EyeControls v-if="tab === 'modes'" :page="page" only="modes" dense />
      <div v-else-if="tab === 'scenes'" class="stack">
        <div class="row between">
          <span class="muted" style="font-size: 13px">当前：{{ eye.activeScene ?? '无场景' }}</span>
          <MdButton variant="outlined" :disabled="!eye.activeScene" @click="eye.setScene(null)">退出</MdButton>
        </div>
        <EyeControls :page="page" only="scenes" dense />
      </div>
      <template v-else>
        <EyeStatusCard :page="page" />
        <EyeControls :page="page" only="light" />
        <WidgetGrid :form-factor="ff.formFactor.value" />
      </template>
    </div>

    <BottomSheet :open="sceneSheet" title="切换场景" @close="sceneSheet = false">
      <div v-for="g in page.sceneGroups" :key="g.group" class="sheet-group">
        <p class="section-title">{{ g.group }}</p>
        <div class="sheet-grid">
          <button
            v-for="s in g.scenes"
            :key="s.id"
            class="sheet-scene"
            :class="{ on: eye.activeScene === s.id }"
            @click="eye.toggleScene(s.id); sceneSheet = false"
          >
            <UiIcon :name="s.icon" :size="22" />
            <span>{{ s.label }}</span>
          </button>
        </div>
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.eye-phone { display: flex; flex-direction: column; gap: 12px; padding: 4px 14px 28px; }
.stage-wrap { --eye-stage-max-height: 46vh; padding-top: 6px; }
.quick-strip {
  display: flex;
  gap: 8px;
  overflow-x: auto;
  padding: 2px 0 4px;
  scrollbar-width: none;
}
.quick-strip::-webkit-scrollbar { display: none; }
.quick {
  flex: none;
  padding: 8px 14px;
  border-radius: 999px;
  border: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  display: inline-flex;
  align-items: center;
  gap: 6px;
}
.quick.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.quick.more { background: var(--md-primary-container); color: var(--md-on-primary-container); border-color: transparent; }
.body { display: flex; flex-direction: column; gap: 12px; }
.sheet-group { margin-bottom: 10px; }
.sheet-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }
.sheet-scene {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 12px 4px;
  border-radius: var(--radius-m);
  border: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 12px var(--font-body);
}
.sheet-scene.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
</style>
