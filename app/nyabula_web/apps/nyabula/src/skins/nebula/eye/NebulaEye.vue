<script setup lang="ts">
/* Eyes first, controls below, optional device-screen feed last. */
import { computed, inject, ref } from 'vue';
import { UiIcon } from '@nyabula/ui';
import WidgetGrid from '../../../components/WidgetGrid.vue';
import type { useFormFactor } from '../../../composables/useFormFactor';
import { useEyePage } from '../../../views/eye/eye.logic';
import { MODE_LABELS, SCENE_META } from '../../../stores/eye';
import EyeStatusCard from '../../../views/eye/EyeStatusCard.vue';
import EyeMirrorCard from '../../../views/eye/EyeMirrorCard.vue';
import EyeControls from '../../../views/eye/EyeControls.vue';
import NebulaLens from './NebulaLens.vue';

const page = useEyePage();
const { eye, session } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const stacked = computed(() => ff.formFactor.value === 'phone' || (ff.formFactor.value === 'tablet' && ff.orientation.value === 'portrait'));

type Tab = 'modes' | 'scenes' | 'more';
const tab = ref<Tab>('modes');
const TABS: { id: Tab; label: string }[] = [
  { id: 'modes', label: '表情' },
  { id: 'scenes', label: '场景' },
  { id: 'more', label: '更多' },
];
const tabIndex = computed(() => TABS.findIndex((t) => t.id === tab.value));

const caption = computed(() => {
  if (!eye.ready) return session.state === 'pairing-required' ? '等待配对' : session.connected ? '眼睛服务未启动' : '设备离线';
  if (eye.activeScene) return '场景 · ' + (SCENE_META[eye.activeScene]?.label ?? eye.activeScene);
  return '表情 · ' + (MODE_LABELS[eye.activeMode] ?? eye.activeMode);
});

</script>

<template>
  <div class="nb-eye" :class="{ stacked }">
    <section class="stage nb-in">
      <div class="lens-box">
        <NebulaLens
          :ready="page.canvasReady.value"
          :show-pair="page.showPairOverlay.value"
          :pairing="page.pairing.value"
          :pair-error="page.pairError.value"
          @ready="page.onEngineReady"
          @look="page.onLook"
          @pair="page.doPair"
        />
      </div>
      <div class="cap">
        <span class="nb-tag" :class="eye.ready ? 'ok' : ''"><span class="nb-dot" />{{ eye.ready ? '实时同步到双屏' : '未同步' }}</span>
        <Transition name="nb-pop" mode="out-in"><span :key="caption" class="cap-mode nb-title">{{ caption }}</span></Transition>
        <p v-if="!session.canControl && session.connected" class="cap-hint">当前角色只读，无法控制表情与场景</p>
      </div>
    </section>

    <section class="ctrl nb-glass nb-in" :class="`tab-${tab}`" style="--d: 120ms">
      <div class="nb-seg" :style="{ '--n': TABS.length, '--i': tabIndex }">
        <span class="seg-light" />
        <button v-for="t in TABS" :key="t.id" :class="{ on: tab === t.id }" @click="tab = t.id">{{ t.label }}</button>
      </div>

      <EyeControls :page="page" :only="tab === 'more' ? 'light' : tab" hide-mirror /><EyeStatusCard v-if="tab === 'more'" :page="page" />

      <div class="actions">
        <button class="nb-btn primary" :disabled="!session.canControl" @click="eye.setMode(eye.activeMode === 'sleep' ? 'idle' : 'sleep')">
          <UiIcon name="dark_mode" :size="17" /> {{ eye.activeMode === 'sleep' ? '叫醒它' : '哄睡' }}
        </button>
        <button v-if="eye.activeScene" class="nb-btn" @click="eye.setScene(null)"><UiIcon name="close" :size="17" /> 退出场景</button>
      </div>
    </section>

    <WidgetGrid class="widgets" :form-factor="ff.formFactor.value" />
    <EyeMirrorCard v-if="!eye.nativeQr" class="screen-feed" />
  </div>
</template>

<style scoped>
.nb-eye { display:flex; flex-direction:column; gap:24px; padding:12px clamp(16px,3vw,36px) 28px; max-width:1120px; margin:0 auto; width:100%; }
.stage { display:flex; flex-direction:column; align-items:center; gap:12px; min-width:0; }
.lens-box { width:min(880px,100%); height:clamp(260px,38vh,420px); }
.cap { text-align:center; display:flex; align-items:center; justify-content:center; gap:12px; flex-wrap:wrap; }
.cap-mode { font-size:20px; color:var(--nb-fg); }
.cap-hint { margin:0; color:var(--nb-sub); font-size:13px; flex-basis:100%; }
.ctrl { padding:20px; display:flex; flex-direction:column; gap:18px; min-width:0; }
.ctrl>.nb-seg { align-self:center; width:min(480px,100%); }
.tab-modes :deep(.controls) { display:grid; grid-template-columns:1fr 1fr; align-items:start; }
.actions { display:flex; gap:10px; flex-wrap:wrap; }
.screen-feed { margin-top:8px; }
.stacked .lens-box { height:clamp(220px,32vh,340px); }
@media(max-width:760px) { .tab-modes :deep(.controls) { grid-template-columns:1fr; }.ctrl { padding:14px; }.nb-eye { gap:18px; } }
</style>
