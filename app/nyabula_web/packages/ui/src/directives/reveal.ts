/* v-reveal: adds .reveal on mount and .reveal-in when scrolled into view. */
import type { Directive } from 'vue';

const observed = new WeakMap<Element, IntersectionObserver>();

export const vReveal: Directive<HTMLElement, number | undefined> = {
  mounted(el, binding) {
    el.classList.add('reveal');
    if (typeof binding.value === 'number') el.style.transitionDelay = `${binding.value}ms`;
    if (!('IntersectionObserver' in window)) {
      el.classList.add('reveal-in');
      return;
    }
    const io = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          if (entry.isIntersecting) {
            el.classList.add('reveal-in');
            io.disconnect();
            observed.delete(el);
          }
        }
      },
      { threshold: 0.12 },
    );
    io.observe(el);
    observed.set(el, io);
  },
  unmounted(el) {
    observed.get(el)?.disconnect();
    observed.delete(el);
  },
};
