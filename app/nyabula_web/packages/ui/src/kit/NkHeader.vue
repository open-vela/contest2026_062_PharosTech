<script setup lang="ts">
/* Feature page header: large rounded icon block, title, subtitle/status and a
 * trailing slot for actions (Mi Home style). */
import UiIcon from '../components/UiIcon.vue';

export interface NkHeaderProps {
  icon?: string;
  title: string;
  subtitle?: string;
  /** Tone of the subtitle (status colouring). */
  tone?: 'default' | 'ok' | 'warn' | 'error';
}
withDefaults(defineProps<NkHeaderProps>(), { tone: 'default' });
</script>

<template>
  <header class="nk-header">
    <div v-if="icon" class="nk-header-icon">
      <UiIcon :name="icon" :size="30" />
    </div>
    <div class="nk-header-main">
      <h2 class="nk-header-title">{{ title }}</h2>
      <p v-if="subtitle" class="nk-header-sub" :class="'tone-' + tone">{{ subtitle }}</p>
    </div>
    <div v-if="$slots.default" class="nk-header-trail"><slot /></div>
  </header>
</template>

<style scoped>
.nk-header { display: flex; align-items: center; gap: 14px; min-height: 44px; }
.nk-header-icon {
  width: 56px; height: 56px; flex: none;
  display: grid; place-items: center;
  border-radius: var(--radius-m);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.nk-header-main { flex: 1; min-width: 0; }
.nk-header-title { margin: 0; font: 600 20px var(--font-title); color: var(--md-on-surface); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nk-header-sub { margin: 3px 0 0; font: 500 13px var(--font-body); color: var(--md-on-surface-variant); }
.nk-header-sub.tone-ok { color: var(--md-success); }
.nk-header-sub.tone-warn { color: var(--md-warning); }
.nk-header-sub.tone-error { color: var(--md-error); }
.nk-header-trail { display: flex; align-items: center; gap: 8px; flex: none; }
</style>
