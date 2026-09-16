/* Developer mode: persisted flag. When on, device pages can be opened
 * without a connection under the virtual key `dev:preview`, so every page
 * (and its empty/offline states) can be inspected. */
import { defineStore } from 'pinia';
import { ref, watch } from 'vue';

export const DEV_KEY = 'nyabula.dev';
export const PREVIEW_KEY = 'dev:preview';

export function isPreviewKey(key: string | null | undefined): boolean {
  return typeof key === 'string' && key.startsWith('dev:');
}

export const useDevStore = defineStore('dev', () => {
  const enabled = ref(localStorage.getItem(DEV_KEY) === '1');
  watch(enabled, (v) => {
    if (v) localStorage.setItem(DEV_KEY, '1');
    else localStorage.removeItem(DEV_KEY);
  });
  return { enabled };
});
