<script setup lang="ts">
/* Phone frame of the bare pages (usually opened from the QR code): a compact
 * brand row and everything top-aligned in one full-width column, so the
 * field and the submit button sit in the part of the screen the keyboard
 * leaves visible. Nothing is pinned to the bottom edge. */
import { NyabulaLogo, ThemeSwitcher } from '@nyabula/ui';

defineProps<{ title: string; sub?: string; wide?: boolean }>();
</script>

<template>
  <div class="gate-phone">
    <header class="brand-row">
      <span class="mark"><NyabulaLogo :size="28" /></span>
      <span class="brand">Nyabula</span>
    </header>
    <div class="head">
      <h1 class="title">{{ title }}</h1>
      <p v-if="sub" class="sub">{{ sub }}</p>
    </div>
    <slot />
    <footer class="theme"><ThemeSwitcher /></footer>
  </div>
</template>

<style scoped>
.gate-phone {
  min-height: 100%;
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: calc(10px + var(--safe-t)) 16px calc(24px + var(--safe-b));
  background:
    radial-gradient(90% 28% at 50% 0%, rgba(var(--md-primary-rgb), 0.14), transparent 75%),
    var(--md-surface);
}
.brand-row { display: flex; align-items: center; gap: 10px; height: 44px; }
.mark { width: 40px; height: 40px; border-radius: 13px; display: grid; place-items: center; color: var(--md-primary); background: rgba(var(--md-primary-rgb), 0.12); }
.brand { font: 600 17px var(--font-title); color: var(--md-on-surface); }
.title { font: 600 22px var(--font-title); color: var(--md-on-surface); margin: 0; }
.sub { font-size: 13.5px; line-height: 1.6; color: var(--md-on-surface-variant); margin: 4px 0 0; }
.theme { display: flex; justify-content: center; margin-top: 8px; }
/* 16px inputs keep iOS from zooming the page on focus; full-height tap targets. */
.gate-phone :deep(.tf input) { font-size: 16px; padding-block: 13px; }
.gate-phone :deep(.md-btn.submit) { padding-block: 15px; font-size: 15px; }
</style>
