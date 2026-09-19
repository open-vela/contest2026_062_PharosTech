/* Routes: /connect, /provision, /d/:key/* device workspace, /account/*, /settings.
 * Guards: device routes require a session (auto-connect from the key).
 * Device build (__NYA_DEVICE__): hash history, because the device's static
 * server only serves files; `connect` is a boot view instead of the address
 * form, and a `?token=` from the provisioning QR code is adopted and stripped. */
import { createRouter, createWebHashHistory, createWebHistory, type RouteRecordRaw } from 'vue-router';
import { useLoadingStore } from '@nyabula/ui';
import { SELF_KEY, useSessionStore } from '../stores/session';
import { isPreviewKey, useDevStore } from '../stores/dev';
import { takeTokenFromQuery } from '../lib/deviceToken';

/* A compile-time constant, so the unused branch (and its chunk) is dropped. */
const connectRoute: RouteRecordRaw = __NYA_DEVICE__
  ? { path: '/connect', name: 'connect', component: () => import('../views/provision/DeviceBootView.vue'), meta: { title: '正在连接' } }
  : { path: '/connect', name: 'connect', component: () => import('../views/connect/ConnectView.vue'), meta: { title: '连接设备' } };

const routes: RouteRecordRaw[] = [
  { path: '/', redirect: () => ({ name: 'connect' }) },
  connectRoute,
  { path: '/provision', name: 'provision', component: () => import('../views/provision/ProvisionView.vue'), meta: { title: '配置 WiFi', quietLink: true } },
  {
    path: '/d/:key',
    component: () => import('../views/DeviceWorkspace.vue'),
    props: true,
    children: [
      { path: '', name: 'home', component: () => import('../views/home/HomeView.vue'), meta: { title: '主页', nav: 'home' } },
      { path: 'eye', name: 'eye', component: () => import('../views/eye/EyeView.vue'), meta: { title: '眼睛控制', nav: 'home', depth: 1 } },
      { path: 'features', name: 'services', component: () => import('../views/services/ServicesView.vue'), meta: { title: '功能', nav: 'services' } },
      { path: 'features/:type', name: 'service', component: () => import('../views/services/ServiceDetailView.vue'), props: true, meta: { title: '功能', nav: 'services', depth: 1 } },
      { path: 'plugins', name: 'plugins', component: () => import('../views/plugins/PluginsView.vue'), meta: { title: '插件', nav: 'plugins' } },
      { path: 'plugins/:id', name: 'plugin', component: () => import('../views/plugins/PluginPageView.vue'), props: true, meta: { title: '插件', nav: 'plugins', depth: 1 } },
      { path: 'plugins/:id/permissions', name: 'plugin-permissions', component: () => import('../views/plugins/PluginPermissionsView.vue'), props: true, meta: { title: '插件权限', nav: 'plugins', depth: 2 } },
      { path: 'settings', name: 'workspace-settings', component: () => import('../views/settings/WorkspaceSettingsView.vue'), meta: { title: '设置', nav: 'workspace-settings' } },
      { path: 'device/:section?', name: 'device', component: () => import('../views/device/DeviceView.vue'), props: true, meta: { title: '设备设置', nav: 'workspace-settings', depth: 1 } },
      { path: 'nyabot', alias: 'agent', name: 'agent', component: () => import('../views/agent/AgentView.vue'), meta: { title: 'Nyabot', nav: 'agent' } },
    ],
  },
  { path: '/account', name: 'account', component: () => import('../views/account/AccountView.vue'), meta: { title: '账号', nav: 'account' } },
  { path: '/account/devices/:id', name: 'account-device', component: () => import('../views/account/AccountDeviceView.vue'), props: true, meta: { title: '设备统计', nav: 'account', depth: 1 } },
  { path: '/settings', name: 'settings', component: () => import('../views/settings/SettingsView.vue'), meta: { title: '客户端设置', depth: 1 } },
  { path: '/:pathMatch(.*)*', redirect: '/connect' },
];

export const router = createRouter({
  history: __NYA_DEVICE__ ? createWebHashHistory() : createWebHistory(),
  routes,
  scrollBehavior: () => ({ top: 0 }),
});

/** Depth of a route for stack transitions (phone). */
export function routeDepth(meta: Record<string, unknown>): number {
  return typeof meta.depth === 'number' ? meta.depth : 0;
}

router.beforeEach(async (to) => {
  const session = useSessionStore();
  if (__NYA_DEVICE__) {
    // Adopt the QR-code token, then drop it from the address bar and history.
    const { token, present, rest } = takeTokenFromQuery(to.query);
    if (present) {
      if (token) session.adoptToken(SELF_KEY, token);
      return { path: to.path, query: rest, hash: to.hash, replace: true };
    }
  }
  const key = typeof to.params.key === 'string' ? to.params.key : null;
  if (key && isPreviewKey(key)) {
    // Developer preview: pages render with their offline/empty states.
    if (!useDevStore().enabled) return { name: 'connect', query: { stay: '1' } };
    if (session.deviceKey !== key) session.enterPreview(key);
  } else if (key) {
    // Auto-connect from the URL so deep links work; ConnectView handles failures.
    if (session.deviceKey !== key || session.state === 'idle' || (session.state === 'closed' && !session.authRequired)) {
      if (!session.connect(key)) return { name: 'connect' };
    }
  }
  const loading = useLoadingStore();
  // Curtain only for top-level section changes, not sibling detail pushes.
  const topChange = (to.meta.nav ?? to.name) !== (router.currentRoute.value.meta.nav ?? router.currentRoute.value.name);
  if (topChange && router.currentRoute.value.name !== undefined && loading.bootDone) {
    // Wait until the curtain fully covers the old page before swapping.
    await loading.startRoute();
  }
  return true;
});

router.afterEach((to) => {
  const loading = useLoadingStore();
  document.title = to.meta.title ? `${to.meta.title} · Nyabula` : 'Nyabula';
  if (loading.routeLoading) loading.scheduleFinish(performance.now() + 450);
});
