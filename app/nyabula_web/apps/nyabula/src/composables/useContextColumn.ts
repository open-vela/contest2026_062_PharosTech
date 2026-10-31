/* Desktop context column state shared by every desktop shell. On wide
 * windows it follows the remembered preference; below ROOMY_PX the page
 * needs the width, so the column starts closed and the toggle opens it for
 * this visit only. It is shown only when a page contributes to it. */
import { computed, inject, ref, type Ref } from 'vue';
import { contextPresence } from './contextPresence';
import type { useFormFactor } from './useFormFactor';

const CTX_KEY = 'nyabula.contextOpen';
/** Narrowest viewport that fits rail + page + 340px column comfortably. */
export const ROOMY_PX = 1240;

export function useContextColumn(inDevice: Ref<boolean>) {
  const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
  const preferred = ref(localStorage.getItem(CTX_KEY) !== '0');
  const narrowOpen = ref(false);
  const roomy = computed(() => ff.viewport.width >= ROOMY_PX);
  const open = computed(() => (roomy.value ? preferred.value : narrowOpen.value));
  const hasContext = computed(() => contextPresence.count > 0);
  const show = computed(() => inDevice.value && hasContext.value && open.value);

  function toggle(): void {
    if (!roomy.value) {
      narrowOpen.value = !narrowOpen.value;
      return;
    }
    preferred.value = !preferred.value;
    localStorage.setItem(CTX_KEY, preferred.value ? '1' : '0');
  }

  return { open, show, hasContext, toggle };
}
