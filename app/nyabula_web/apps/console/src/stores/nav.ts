/* Hash navigation: reloadable pages and browser back without server rewrites. */
import { defineStore } from 'pinia';
import { ref } from 'vue';

export type Page = 'overview' | 'devices' | 'device' | 'traffic' | 'events' | 'account';

export const useNavStore = defineStore('nav', () => {
  const page = ref<Page>('overview');
  const deviceId = ref<string | null>(null);

  function readLocation(): void {
    const parts = window.location.hash.replace(/^#\/?/, '').split('/');
    const target = parts[0];
    const pages: Page[] = ['overview', 'devices', 'traffic', 'events', 'account'];
    if (target === 'device' && parts[1]) {
      try {
        deviceId.value = decodeURIComponent(parts[1]);
        page.value = 'device';
        return;
      } catch { /* Invalid encoded URL goes to the overview. */ }
    }
    page.value = pages.includes(target as Page) ? target as Page : 'overview';
    deviceId.value = null;
  }
  readLocation();
  window.addEventListener('hashchange', readLocation);

  function go(p: Page, id?: string): void {
    page.value = p === 'device' && !id ? 'devices' : p;
    deviceId.value = p === 'device' ? (id ?? null) : null;
    const hash = `#/${page.value}${page.value === 'device' && id ? '/' + encodeURIComponent(id) : ''}`;
    if (location.hash !== hash) location.hash = hash;
  }

  return { page, deviceId, go };
});
