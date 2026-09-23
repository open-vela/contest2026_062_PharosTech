/* Routes: /connect, /provision, /d/:key/* device workspace, /account/*, /settings.
 * Guards: device routes require a session (auto-connect from the key).
 * Device build (__NYA_DEVICE__): hash history, because the device's static
 * server only serves files; `connect` is a boot view instead of the address
 * form. A `?token=` from the QR code is the PAIR token: adopted for this tab
 * only (never stored) and stripped. /login and /setup-password exist only
 * here, and the access store decides which single page may be shown: the
 * gate pages until authenticated, then /provision until the device is online.
 * `meta.bare` routes render without any app shell. */
import { createRouter, createWebHashHistory, createWebHistory, type RouteRecordRaw } from 'vue-router';
import { useLoadingStore } from '@nyabula/ui';
import { SELF_KEY, useSessionStore } from '../stores/session';
import { isPreviewKey, useDevStore } from '../stores/dev';
import { useDeviceAccessStore } from '../stores/deviceAccess';
import { takeTokenFromQuery } from '../lib/deviceToken';
import { guardRedirect } from '../lib/deviceAccess';

/* A compile-time constant, so the unused branch (and its chunk) is dropped. */
const connectRoute: RouteRecordRaw = __NYA_DEVICE__
  ? { path: '/connect', name: 'connect', component: () => import('../views/provision/DeviceBootView.vue'), meta: { title: '正在连接', bare: true } }
  : { path: '/connect', name: 'connect', component: () => import('../views/connect/ConnectView.vue'), meta: { title: '连接设备' } };

/* Password gate: only the device build has these pages (and their chunks). */
const accessRoutes: RouteRecordRaw[] = __NYA_DEVICE__
  ? [
      { path: '/login', name: 'login', component: () => import('../views/access/LoginView.vue'), meta: { title: '登录', bare: true } },
      { path: '/setup-password', name: 'setup-password', component: () => import('../views/access/SetupPasswordView.vue'), meta: { title: '设置访问密码', bare: true } },
    ]
  : [];

const routes: RouteRecordRaw[] = [
  { path: '/', redirect: () => ({ name: 'connect' }) },
  connectRoute,
  { path: '/provision', name: 'provision', component: () => import('../views/provision/ProvisionView.vue'), meta: { title: '配置 WiFi', quietLink: true, bare: __NYA_DEVICE__ } },
  ...accessRoutes,
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
      // Its own address, for links and docs; the page itself is a section of the device settings.
      { path: 'models', name: 'models', redirect: (to) => ({ name: 'device', params: { key: to.params.key, section: 'models' } }) },
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
    const access = useDeviceAccessStore();
    // Adopt the QR-code pair token (memory only), then drop it from the address bar and history.
    const { token, present, rest } = takeTokenFromQuery(to.query);
    if (present) {
      if (token && session.adoptPairToken(token)) access.restart();
      return { path: to.path, query: rest, hash: to.hash, replace: true };
    }
    // Reads cached state only: the boot view decides once, nothing waits here.
    const target = guardRedirect(typeof to.name === 'string' ? to.name : null, access.phase, access.online);
    if (target) {
      if (access.phase === 'boot') access.rememberPath(to.fullPath);
      return target === 'home' ? { name: 'home', params: { key: SELF_KEY }, replace: true } : { name: target, replace: true };
    }
    // The page the device serves only ever talks to that device: a foreign
    // key would say hello without a credential and end in an auth failure.
    const foreign = typeof to.params.key === 'string' && to.params.key !== SELF_KEY && !isPreviewKey(to.params.key);
    if (foreign && to.name) return { name: to.name, params: { ...to.params, key: SELF_KEY }, query: to.query, replace: true };
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
  if (topChange && router.currentRoute.value.name !== undefined && loading.bootDone && to.meta.bare !== true) {
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
