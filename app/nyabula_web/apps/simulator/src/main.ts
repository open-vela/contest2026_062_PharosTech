import { createApp } from 'vue';
import { createPinia } from 'pinia';
import '@nyabula/ui/styles/tokens.css';
import './style.css';
import App from './App.vue';
createApp(App).use(createPinia()).mount('#app');
