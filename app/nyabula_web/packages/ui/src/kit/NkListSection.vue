<script setup lang="ts">
/* Grouped list: section title (+ optional trailing slot) and a rounded
 * container for NkRow / NkToggleRow children. */
export interface NkListSectionProps {
  title?: string;
  /** Wrap children in a surface container card. */
  card?: boolean;
}
withDefaults(defineProps<NkListSectionProps>(), { card: true });
</script>

<template>
  <section class="nk-list-section">
    <div v-if="title || $slots.trailing" class="nk-list-head">
      <h4 v-if="title" class="nk-list-title">{{ title }}</h4>
      <div v-if="$slots.trailing" class="nk-list-trail"><slot name="trailing" /></div>
    </div>
    <div class="nk-list-body" :class="{ card }">
      <slot />
    </div>
  </section>
</template>

<style scoped>
.nk-list-section { display: flex; flex-direction: column; gap: 8px; }
.nk-list-head { display: flex; align-items: center; justify-content: space-between; padding: 0 12px; min-height: 24px; }
.nk-list-title { margin: 0; font: 600 12px var(--font-body); letter-spacing: 1px; color: var(--md-on-surface-variant); }
.nk-list-trail { display: flex; align-items: center; gap: 8px; }
.nk-list-body { display: flex; flex-direction: column; gap: 2px; }
.nk-list-body.card { background: var(--md-surface-container); border-radius: var(--radius-l); padding: 6px; }
</style>
