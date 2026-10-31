<script setup lang="ts">
/* Bare shell for `meta.bare` routes (device build: boot, login, password
 * setup, WiFi provisioning). Deliberately no navigation of any kind, no
 * device switcher, no search and no connection banner: these pages are
 * sealed, there is nowhere else to go until they are done. Branding and the
 * theme control belong to the per-form-factor gate frames
 * (components/gate); this shell only scrolls and keeps the focused field and
 * the submit button clear of the on-screen keyboard. */
import { nextTick, ref, watch } from 'vue';
import { RouterView } from 'vue-router';
import { useKeyboardInset } from '../composables/useKeyboardInset';

const kb = useKeyboardInset();
const main = ref<HTMLElement | null>(null);

/* Keyboard just opened: bring the whole form (its submit button included)
 * into the visible area, falling back to the focused field. */
watch(kb, async (now, before) => {
  if (now <= before) return;
  await nextTick();
  const active = document.activeElement;
  if (!(active instanceof HTMLElement) || !main.value?.contains(active)) return;
  const form = active.closest('form');
  const room = main.value.clientHeight - now;
  const target = form && form.offsetHeight <= room ? form : active;
  target.scrollIntoView({ block: target === form ? 'end' : 'center', behavior: 'smooth' });
});
</script>

<template>
  <div class="bare">
    <main ref="main" class="bare-main" :style="{ paddingBottom: `${kb}px`, scrollPaddingBottom: `${kb + 12}px` }">
      <RouterView v-slot="{ Component }">
        <Transition name="page" mode="out-in">
          <component :is="Component" />
        </Transition>
      </RouterView>
    </main>
  </div>
</template>

<style scoped>
.bare { height: 100dvh; background: var(--md-surface); }
.bare-main { height: 100%; overflow-y: auto; position: relative; }
</style>
