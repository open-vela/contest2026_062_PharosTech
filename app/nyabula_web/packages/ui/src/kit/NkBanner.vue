<script setup lang="ts">
/* Inline notice with tone, optional icon, action and close. */
import UiIcon from '../components/UiIcon.vue';

export interface NkBannerProps {
  text: string;
  title?: string;
  tone?: 'info' | 'ok' | 'warn' | 'error';
  icon?: string;
  closable?: boolean;
  actionText?: string;
}
withDefaults(defineProps<NkBannerProps>(), { tone: 'info', closable: false });
const emit = defineEmits<{ (e: 'close'): void; (e: 'action'): void }>();
const TONE_ICON: Record<string, string> = { info: 'info', ok: 'check_circle', warn: 'warning', error: 'error' };
</script>

<template>
  <div class="nk-banner" :class="'tone-' + tone" role="status">
    <UiIcon :name="icon || TONE_ICON[tone]" :size="20" class="nk-banner-icon" />
    <div class="nk-banner-main">
      <div v-if="title" class="nk-banner-title">{{ title }}</div>
      <div class="nk-banner-text">{{ text }}</div>
    </div>
    <button v-if="actionText" type="button" class="nk-banner-action" @click="emit('action')">{{ actionText }}</button>
    <button v-if="closable" type="button" class="nk-banner-close" aria-label="关闭" @click="emit('close')">
      <UiIcon name="close" :size="18" />
    </button>
  </div>
</template>

<style scoped>
.nk-banner {
  display: flex; align-items: center; gap: 10px;
  padding: 10px 12px; min-height: 44px;
  border-radius: var(--radius-m);
  font: 500 13px var(--font-body);
}
.nk-banner-icon { flex: none; }
.nk-banner-main { flex: 1; min-width: 0; }
.nk-banner-title { font-weight: 600; margin-bottom: 2px; }
.nk-banner-text { line-height: 1.4; }
.nk-banner-action, .nk-banner-close {
  border: none; background: transparent; color: inherit; cursor: pointer;
  min-height: 36px; padding: 6px 10px; border-radius: var(--radius-full);
  font: 600 13px var(--font-body);
}
.nk-banner-close { width: 36px; padding: 0; display: grid; place-items: center; }
.nk-banner-action:hover, .nk-banner-close:hover { background: rgba(127, 127, 127, 0.15); }
.tone-info { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.tone-ok { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.tone-warn { background: rgba(255, 217, 77, 0.18); color: var(--md-warning); }
.tone-error { background: rgba(255, 180, 171, 0.15); color: var(--md-error); }
</style>
