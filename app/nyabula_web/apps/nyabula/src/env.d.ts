/// <reference types="vite/client" />
declare module '*.vue' {
  import type { DefineComponent } from 'vue';
  const component: DefineComponent<object, object, unknown>;
  export default component;
}

/** True only in the `device` build mode (site served by the device itself). */
declare const __NYA_DEVICE__: boolean;
