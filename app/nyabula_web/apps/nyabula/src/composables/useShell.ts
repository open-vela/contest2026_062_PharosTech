/* Shell helpers shared by the three layouts: current nav id, device-scoped
 * navigation, connection tone/label. */
import { computed } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { PRIMARY_NAV, type NavItem } from '../nav';
import { useSessionStore } from '../stores/session';
import { PREVIEW_KEY, useDevStore } from '../stores/dev';

export function useShell() {
  const route = useRoute();
  const router = useRouter();
  const session = useSessionStore();
  const dev = useDevStore();

  const activeNav = computed(() => (route.meta.nav as string | undefined) ?? (route.name === 'connect' ? 'connect' : undefined));
  const inDevice = computed(() => typeof route.params.key === 'string');
  const deviceKey = computed(() => session.deviceKey ?? (dev.enabled ? PREVIEW_KEY : session.lastDeviceKey));

  function go(item: NavItem): void {
    if (item.to.startsWith('/')) {
      void router.push(item.to);
      return;
    }
    const key = deviceKey.value;
    if (!key) {
      void router.push({ name: 'connect' });
      return;
    }
    void router.push({ name: item.to, params: { key } });
  }

  const connTone = computed<'ok' | 'busy' | 'off'>(() => {
    switch (session.state) {
      case 'connected':
        return 'ok';
      case 'connecting':
      case 'authenticating':
      case 'reconnecting':
      case 'pairing-required':
        return 'busy';
      default:
        return 'off';
    }
  });
  const connLabel = computed(() => {
    const labels: Record<string, string> = {
      idle: '未连接',
      connecting: '连接中',
      authenticating: '认证中',
      'pairing-required': '待配对',
      connected: session.device?.name ?? '已连接',
      reconnecting: '重连中',
      closed: '已断开',
    };
    return labels[session.state] ?? session.state;
  });

  const title = computed(() => (route.meta.title as string | undefined) ?? 'Nyabula');
  const depth = computed(() => (typeof route.meta.depth === 'number' ? route.meta.depth : 0));

  function back(): void {
    if (window.history.length > 1) router.back();
    else if (activeNav.value && deviceKey.value) void router.push({ name: activeNav.value, params: { key: deviceKey.value } });
    else void router.push({ name: 'connect' });
  }

  return { nav: PRIMARY_NAV, activeNav, inDevice, deviceKey, go, connTone, connLabel, title, depth, back, session, route };
}
