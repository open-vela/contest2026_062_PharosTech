/* On-screen keyboard inset. Mobile browsers shrink only the VISUAL viewport
 * when the keyboard opens, so a 100dvh scroll container keeps its height and
 * its lower part ends up under the keyboard. The returned inset (px) is
 * applied as bottom padding by the bare shell, which makes everything
 * scrollable into the visible area. 0 on desktop and where unsupported. */
import { onBeforeUnmount, onMounted, ref } from 'vue';
import { keyboardInset } from '../lib/keyboardInset';

export function useKeyboardInset() {
  const inset = ref(0);
  const vv = typeof window !== 'undefined' ? window.visualViewport : null;
  function measure(): void {
    if (vv) inset.value = keyboardInset(window.innerHeight, vv.height, vv.offsetTop);
  }
  onMounted(() => {
    vv?.addEventListener('resize', measure);
    vv?.addEventListener('scroll', measure);
    measure();
  });
  onBeforeUnmount(() => {
    vv?.removeEventListener('resize', measure);
    vv?.removeEventListener('scroll', measure);
  });
  return inset;
}
