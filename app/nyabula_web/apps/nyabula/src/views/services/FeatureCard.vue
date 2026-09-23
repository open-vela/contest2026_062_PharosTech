<script setup lang="ts">
/* Medium feature card: header (icon, name, live status, open) + the
 * feature's own compact control (<Name>Mini.vue) so common actions happen
 * right here; the full page is one tap away. A planned feature is drawn
 * dimmed with a tag and what it waits for; its control is never mounted, so
 * it cannot reach the device. */
import { defineAsyncComponent, type Component } from 'vue';
import { Skeleton, UiIcon } from '@nyabula/ui';
import type { FeatureDef } from './features/contract';
import type { FormFactor } from '../../composables/useFormFactor';

const props = defineProps<{
  def: FeatureDef;
  active: boolean;
  sub: string;
  ff: FormFactor;
  payload: Record<string, unknown> | null;
}>();
const emit = defineEmits<{ (e: 'open'): void }>();

const planned = props.def.stage === 'planned';
const Mini: Component | null = props.def.mini && !planned
  ? defineAsyncComponent({ loader: props.def.mini, loadingComponent: Skeleton, delay: 120 })
  : null;
</script>

<template>
  <section class="fc" :class="{ active, planned }" :aria-disabled="planned || undefined">
    <header class="fc-head">
      <button class="fc-id" @click="emit('open')">
        <span class="fc-icon"><UiIcon :name="def.icon" :size="20" /></span>
        <span class="fc-text">
          <span class="fc-title">{{ def.label }}</span>
          <span class="fc-sub" :class="{ live: active }">{{ sub }}</span>
        </span>
      </button>
      <span v-if="planned" class="fc-tag">规划中</span>
      <span v-else-if="active" class="fc-live"><span class="dot" />显示中</span>
      <button v-if="!planned" class="fc-more" aria-label="详情" title="打开详情" @click="emit('open')"><UiIcon name="fullscreen" :size="18" /></button>
    </header>
    <div class="fc-body">
      <template v-if="planned">
        <p class="fc-needs">{{ def.needs ?? '等待硬件与驱动接入' }}</p>
        <!-- The page exists and explains what is planned; only the device
             side is missing, so it is worth looking at. -->
        <button class="fc-fallback" @click="emit('open')">预览界面 <UiIcon name="chevron_right" :size="16" /></button>
      </template>
      <component :is="Mini" v-else-if="Mini" :type="def.type" :ff="ff" :active="active" :payload="payload" />
      <button v-else class="fc-fallback" @click="emit('open')">打开设置 <UiIcon name="chevron_right" :size="16" /></button>
    </div>
  </section>
</template>

<style scoped>
.fc {
  display: flex;
  flex-direction: column;
  gap: 10px;
  padding: 12px 14px 14px;
  border-radius: var(--radius-l);
  background: var(--md-surface-container);
  border: 1px solid transparent;
  transition: background var(--dur-fast), border-color var(--dur-fast);
  min-height: 150px;
}
.fc.active { border-color: rgba(var(--md-primary-rgb), 0.45); }
/* Planned: present but clearly not usable. */
.fc.planned { min-height: 0; background: transparent; border: 1px dashed var(--md-outline-variant); }
/* Dimmed element by element, not as a whole card: the preview button is the
 * one thing here that does something and must stay legible. */
.fc.planned .fc-head { opacity: 0.58; }
.fc.planned .fc-needs { opacity: 0.58; }
.fc.planned .fc-body { min-height: 0; gap: 8px; align-items: flex-start; }
.fc-tag {
  flex: none; font: 600 11px var(--font-body); padding: 2px 8px; border-radius: 999px;
  background: var(--md-surface-container-highest); color: var(--md-on-surface-variant);
}
.fc-needs { margin: 0; font-size: 12.5px; line-height: 1.5; color: var(--md-on-surface-variant); }
.fc-head { display: flex; align-items: center; gap: 8px; }
.fc-id {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: center;
  gap: 10px;
  border: none;
  background: transparent;
  padding: 0;
  cursor: pointer;
  text-align: left;
  font: inherit;
  color: var(--md-on-surface);
}
.fc-icon {
  width: 38px;
  height: 38px;
  border-radius: 11px;
  display: grid;
  place-items: center;
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  flex: none;
}
.fc.active .fc-icon { background: var(--md-primary); color: var(--md-on-primary); }
.fc-text { display: flex; flex-direction: column; min-width: 0; gap: 1px; }
.fc-title { font: 600 14.5px var(--font-body); }
.fc-sub { font-size: 12px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.fc-sub.live { color: var(--md-primary); }
.fc-live { display: inline-flex; align-items: center; gap: 5px; font-size: 11.5px; font-weight: 600; color: var(--md-primary); flex: none; }
.dot { width: 6px; height: 6px; border-radius: 50%; background: var(--md-primary); box-shadow: 0 0 0 3px rgba(var(--md-primary-rgb), 0.18); }
.fc-more {
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  width: 32px;
  height: 32px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  cursor: pointer;
  flex: none;
}
.fc-more:hover { background: var(--md-surface-container-highest); color: var(--md-on-surface); }
.fc-body { flex: 1; display: flex; flex-direction: column; justify-content: center; min-height: 64px; }
.fc-fallback { border: none; background: transparent; color: var(--md-primary); font: 600 13px var(--font-body); cursor: pointer; display: inline-flex; align-items: center; gap: 2px; padding: 0; }
</style>
