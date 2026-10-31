import { createApp } from 'vue';
import { createPinia } from 'pinia';
import '@nyabula/ui/styles/tokens.css';
import '@nyabula/ui/styles/base.css';
import '@nyabula/ui/styles/motion.css';
import '@nyabula/ui/styles/skins.css';
import { useLoadingStore, useThemeStore } from '@nyabula/ui';
import App from './App.vue';
import { useAuthStore } from './stores/auth';

const app = createApp(App);
app.use(createPinia());

/* Theme (skin x mode x palette) is shared with the device WebUI through
 * localStorage; apply it before the first paint. */
useThemeStore().init();
const loading = useLoadingStore();
loading.setBootLabel('连接 Nyabula Cloud');
loading.setBootProgress(20);

app.mount('#app');

/* Boot: fonts -> session probe (GET /auth/me) -> reveal. */
(async () => {
  await (document.fonts?.ready ?? Promise.resolve());
  loading.setBootProgress(55);
  loading.setBootLabel('恢复登录状态');
  await useAuthStore().init();
  loading.finishBoot();
})();
