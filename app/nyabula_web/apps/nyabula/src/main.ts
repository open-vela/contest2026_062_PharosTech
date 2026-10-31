import { createApp } from 'vue';
import { createPinia } from 'pinia';
import '@nyabula/ui/styles/tokens.css';
import '@nyabula/ui/styles/base.css';
import '@nyabula/ui/styles/motion.css';
import './styles/app.css';
import { useThemeStore, useLoadingStore, vReveal } from '@nyabula/ui';
import App from './App.vue';
import { router } from './router';
import { useAccountStore } from './stores/account';
import { useSessionStore } from './stores/session';

const app = createApp(App);
const pinia = createPinia();
app.use(pinia);
app.use(router);
app.directive('reveal', vReveal);

const theme = useThemeStore();
theme.init();

const loading = useLoadingStore();
loading.setBootProgress(15);

app.mount('#app');

/* Boot sequence: fonts -> cloud session restore -> auto-reconnect last device. */
(async () => {
  loading.setBootLabel('加载字体');
  await (document.fonts?.ready ?? Promise.resolve());
  loading.setBootProgress(45);
  loading.setBootLabel('建立连接');
  await useAccountStore().restore().catch(() => undefined);
  loading.setBootProgress(70);
  const session = useSessionStore();
  await router.isReady();
  if (router.currentRoute.value.name === 'connect' && session.lastDeviceKey && !location.search.includes('stay')) {
    loading.setBootLabel('连接上次的设备');
    // Try the last device briefly; ConnectView shows progress and falls back.
    await router.replace({ name: 'home', params: { key: session.lastDeviceKey } }).catch(() => undefined);
  }
  loading.setBootProgress(100);
  loading.finishBoot();
})();
