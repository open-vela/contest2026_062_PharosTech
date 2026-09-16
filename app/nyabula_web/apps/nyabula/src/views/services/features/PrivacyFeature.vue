<script setup lang="ts">
/* Privacy feature. Camera / microphone / location toggles plus a master
 * privacy mode that overrides them. State is local; pushes
 * { camera, mic, location, privacy_mode } to the eye. */
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkBanner, NkListSection, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

const mem = useFeatureMemory(props.type, { camera: true, mic: true, location: false, privacyMode: false });
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const active = computed(() => eye.activeScene === props.type);
const effective = computed(() => ({
  camera: state.camera && !state.privacyMode,
  mic: state.mic && !state.privacyMode,
  location: state.location && !state.privacyMode,
}));
const onCount = computed(() => Object.values(effective.value).filter(Boolean).length);
const subtitle = computed(() => (state.privacyMode ? '隐私模式已开启，所有感知已关闭' : `${onCount.value} 项感知开启`));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { ...effective.value, privacy_mode: state.privacyMode });
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="privacy" title="隐私" :subtitle="subtitle" :tone="state.privacyMode ? 'warn' : active ? 'ok' : 'default'" />
    <div class="grid">
      <section class="card">
        <div class="master" :class="{ on: state.privacyMode }">
          <span class="master-icon"><UiIcon :name="state.privacyMode ? 'shield' : 'security'" :size="36" /></span>
          <div class="master-text">
            <div class="master-title">隐私模式</div>
            <div class="muted">开启后摄像头、麦克风和位置全部关闭，眼睛会显示遮罩。</div>
          </div>
        </div>
        <NkListSection :card="true">
          <NkToggleRow v-model="state.privacyMode" icon="shield" title="隐私模式总开关" :sub="state.privacyMode ? '已开启' : '未开启'" />
        </NkListSection>
        <NkBanner v-if="state.privacyMode" tone="warn" text="隐私模式下，下方开关暂不生效。" />
        <NkActionBar primary-text="显示到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <NkListSection title="感知权限">
          <NkToggleRow v-model="state.camera" icon="camera" title="摄像头" sub="用于认主、表情识别" :disabled="state.privacyMode" />
          <NkToggleRow v-model="state.mic" icon="mic" title="麦克风" sub="用于语音唤醒与对话" :disabled="state.privacyMode" />
          <NkToggleRow v-model="state.location" icon="language" title="位置" sub="用于天气与本地服务" :disabled="state.privacyMode" />
        </NkListSection>
        <p class="muted small">开关状态保存在本机，<span class="contract-only">设备侧权限同步为契约预留</span>。</p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.master { display: flex; align-items: center; gap: 14px; padding: 8px 4px; }
.master-icon {
  display: inline-flex; align-items: center; justify-content: center; width: 64px; height: 64px; flex: none;
  border-radius: var(--radius-l); background: var(--md-surface-container-high); color: var(--md-on-surface-variant);
  transition: background var(--dur-medium) var(--ease-standard), color var(--dur-medium) var(--ease-standard);
}
.master.on .master-icon { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.master-title { font-size: 18px; font-weight: 700; }
.small { font-size: 12px; margin: 0; }
</style>
