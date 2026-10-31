<script setup lang="ts">
/* Desktop frame of the bare (shell-less) pages: brand mark, title and one
 * card column centred in the window with a fixed max width, theme control
 * underneath. A computer gets here by typing the device address. */
import { NyabulaLogo, ThemeSwitcher } from '@nyabula/ui';

defineProps<{ title: string; sub?: string; wide?: boolean }>();
</script>

<template>
  <div class="gate-desktop">
    <div class="column" :class="{ wide }">
      <header class="hero">
        <span class="mark"><NyabulaLogo :size="56" /></span>
        <span class="brand">Nyabula</span>
        <h1 class="title">{{ title }}</h1>
        <p v-if="sub" class="sub">{{ sub }}</p>
      </header>
      <slot />
      <footer class="theme"><ThemeSwitcher /></footer>
    </div>
  </div>
</template>

<style scoped>
.gate-desktop {
  min-height: 100%;
  display: grid;
  place-items: center;
  padding: 40px 24px;
  background:
    radial-gradient(60% 50% at 50% 0%, rgba(var(--md-primary-rgb), 0.12), transparent 70%),
    var(--md-surface);
}
.column { width: 100%; max-width: 420px; display: flex; flex-direction: column; gap: 18px; }
.column.wide { max-width: 560px; }
.hero { display: flex; flex-direction: column; align-items: center; text-align: center; gap: 6px; margin-bottom: 4px; }
.mark { width: 72px; height: 72px; border-radius: 22px; display: grid; place-items: center; color: var(--md-primary); background: rgba(var(--md-primary-rgb), 0.12); }
.brand { font: 600 13px var(--font-body); letter-spacing: 0.16em; text-transform: uppercase; color: var(--md-on-surface-variant); margin-top: 6px; }
.title { font: 600 24px var(--font-title); color: var(--md-on-surface); margin: 2px 0 0; }
.sub { font-size: 13.5px; line-height: 1.6; color: var(--md-on-surface-variant); margin: 0; max-width: 36em; }
.theme { display: flex; justify-content: center; margin-top: 6px; }
</style>
