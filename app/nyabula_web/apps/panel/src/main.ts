import { createApp } from 'vue';
import { createPinia } from 'pinia';
import App from './App.vue';
import '@nyabula/ui/styles/tokens.css';
import '@nyabula/ui/styles/base.css';

document.documentElement.dataset.theme = 'dark';

createApp(App).use(createPinia()).mount('#app');
