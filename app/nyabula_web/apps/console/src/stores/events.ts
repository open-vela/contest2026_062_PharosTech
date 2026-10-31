import { defineStore } from 'pinia';
import { ref } from 'vue';

export interface ConsoleEvent { id: number; at: string; tone: 'ok' | 'info' | 'warn'; text: string }
/** Observations from this console session, not a server audit history. */
export const useEventsStore = defineStore('console-events', () => {
  const entries = ref<ConsoleEvent[]>([]);
  let next = 0;
  function add(text: string, tone: ConsoleEvent['tone'] = 'info') {
    entries.value.unshift({ id: ++next, at: new Date().toISOString(), tone, text });
    entries.value = entries.value.slice(0, 200);
  }
  function reset() { entries.value = []; }
  return { entries, add, reset };
});
