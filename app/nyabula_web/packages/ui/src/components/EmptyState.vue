<script setup lang="ts">
/* Empty / error placeholder with optional retry. tone=error paints red. */
import UiIcon from './UiIcon.vue';
import MdButton from './MdButton.vue';

defineProps<{
  icon?: string;
  title: string;
  hint?: string;
  tone?: 'neutral' | 'error';
  actionText?: string;
  compact?: boolean;
}>();
const emit = defineEmits<{ (e: 'action'): void }>();
</script>

<template>
  <div class="empty" :class="[tone ?? 'neutral', { compact }]">
    <div class="orb"><UiIcon :name="icon ?? (tone === 'error' ? 'error' : 'auto_awesome')" :size="compact ? 22 : 30" /></div>
    <h4 class="title">{{ title }}</h4>
    <p v-if="hint" class="hint">{{ hint }}</p>
    <MdButton v-if="actionText" variant="tonal" @click="emit('action')">{{ actionText }}</MdButton>
  </div>
</template>

<style scoped>
.empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  text-align: center;
  gap: 10px;
  padding: 40px 20px;
  color: var(--md-on-surface-variant);
}
.empty.compact { padding: 20px 12px; gap: 6px; }
.orb {
  width: 64px;
  height: 64px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: var(--md-surface-container-highest);
  color: var(--md-primary);
}
.compact .orb { width: 44px; height: 44px; }
.error .orb {
  background: color-mix(in srgb, var(--md-error) 14%, var(--md-surface-container-highest));
  color: var(--md-error);
}
.title { margin: 4px 0 0; font: 600 16px var(--font-title); color: var(--md-on-surface); }
.compact .title { font-size: 14px; }
.hint { margin: 0; font-size: 13px; max-width: 360px; line-height: 1.55; }
</style>
