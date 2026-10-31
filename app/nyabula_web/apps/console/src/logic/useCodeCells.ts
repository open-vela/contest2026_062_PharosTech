/* Segmented code input (claim code): typing advances, Backspace on an
 * empty cell steps back, pasting spreads across cells. Digits only. */
import { computed, nextTick, ref } from 'vue';

export function useCodeCells(length = 6) {
  const cells = ref<string[]>(Array.from({ length }, () => ''));
  const els: HTMLInputElement[] = [];

  function bind(i: number) {
    return (el: unknown) => {
      if (el) els[i] = el as HTMLInputElement;
    };
  }
  function onInput(i: number, e: Event): void {
    const input = e.target as HTMLInputElement;
    const v = input.value.replace(/\D/g, '');
    if (v.length > 1) {
      v.slice(0, length - i)
        .split('')
        .forEach((ch, k) => (cells.value[i + k] = ch));
      input.value = cells.value[i] ?? '';
      void nextTick(() => els[Math.min(length - 1, i + v.length)]?.focus());
      return;
    }
    cells.value[i] = v;
    input.value = v;
    if (v && i < length - 1) void nextTick(() => els[i + 1]?.focus());
  }
  function onKey(i: number, e: KeyboardEvent): void {
    if (e.key === 'Backspace' && !cells.value[i] && i > 0) els[i - 1]?.focus();
    else if (e.key === 'ArrowLeft' && i > 0) els[i - 1]?.focus();
    else if (e.key === 'ArrowRight' && i < length - 1) els[i + 1]?.focus();
  }
  function reset(): void {
    cells.value = Array.from({ length }, () => '');
  }
  function focus(i = 0): void {
    void nextTick(() => els[i]?.focus());
  }
  const value = computed(() => cells.value.join(''));
  const complete = computed(() => cells.value.every((c) => c));

  return { cells, bind, onInput, onKey, reset, focus, value, complete, length };
}
