<script setup lang="ts">
/* Tablet frame of the bare pages. Landscape: brand panel on the left, the
 * card on the right, so the form stays in the upper half that the on-screen
 * keyboard leaves visible. Portrait: one centred column with touch-sized
 * spacing, anchored towards the top for the same reason. */
import { computed, inject } from 'vue';
import { NyabulaLogo, ThemeSwitcher } from '@nyabula/ui';
import type { useFormFactor } from '../../composables/useFormFactor';

defineProps<{ title: string; sub?: string; wide?: boolean }>();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor');
const landscape = computed(() => ff?.orientation.value === 'landscape');
</script>

<template>
  <div class="gate-tablet" :class="{ landscape }">
    <header class="hero">
      <span class="mark"><NyabulaLogo :size="landscape ? 72 : 60" /></span>
      <span class="brand">Nyabula</span>
      <h1 class="title">{{ title }}</h1>
      <p v-if="sub" class="sub">{{ sub }}</p>
    </header>
    <div class="column" :class="{ wide }">
      <slot />
      <footer class="theme"><ThemeSwitcher /></footer>
    </div>
  </div>
</template>

<style scoped>
.gate-tablet {
  min-height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 22px;
  padding: calc(9vh + var(--safe-t)) 32px calc(32px + var(--safe-b));
  background:
    radial-gradient(70% 40% at 50% 0%, rgba(var(--md-primary-rgb), 0.12), transparent 70%),
    var(--md-surface);
}
.gate-tablet.landscape {
  display: grid;
  grid-template-columns: minmax(0, 5fr) minmax(0, 6fr);
  align-items: start;
  gap: 40px;
  padding: calc(8vh + var(--safe-t)) 48px calc(32px + var(--safe-b));
  background:
    radial-gradient(50% 70% at 0% 30%, rgba(var(--md-primary-rgb), 0.14), transparent 70%),
    var(--md-surface);
}
.hero { display: flex; flex-direction: column; align-items: center; text-align: center; gap: 6px; }
.landscape .hero { align-items: flex-start; text-align: left; padding-top: 12px; justify-self: end; max-width: 380px; }
.mark { width: 84px; height: 84px; border-radius: 26px; display: grid; place-items: center; color: var(--md-primary); background: rgba(var(--md-primary-rgb), 0.12); }
.landscape .mark { width: 100px; height: 100px; border-radius: 30px; }
.brand { font: 600 13px var(--font-body); letter-spacing: 0.16em; text-transform: uppercase; color: var(--md-on-surface-variant); margin-top: 8px; }
.title { font: 600 26px var(--font-title); color: var(--md-on-surface); margin: 2px 0 0; }
.landscape .title { font-size: 30px; }
.sub { font-size: 14.5px; line-height: 1.65; color: var(--md-on-surface-variant); margin: 0; max-width: 34em; }
.column { width: 100%; max-width: 480px; display: flex; flex-direction: column; gap: 18px; }
.column.wide { max-width: 600px; }
.landscape .column { justify-self: start; }
.theme { display: flex; justify-content: center; margin-top: 4px; }
/* Touch-sized controls inside the slotted body. */
.column :deep(.md-btn.submit) { padding-block: 15px; font-size: 15px; }
.column :deep(.tf input) { padding-block: 13px; font-size: 16px; }
</style>
