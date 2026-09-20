/* Tiny in-app navigation (no router dependency). */
import { defineStore } from 'pinia';
import { ref } from 'vue';

export type Page = 'overview' | 'devices' | 'device';

export const useNavStore = defineStore('nav', () => {
  const page = ref<Page>('overview');
  const deviceId = ref<string | null>(null);

  function go(p: Page, id?: string): void {
    page.value = p;
    deviceId.value = p === 'device' ? (id ?? null) : null;
  }

  return { page, deviceId, go };
});
