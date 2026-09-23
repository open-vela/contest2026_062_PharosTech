import { defineConfig, type Plugin } from 'vite';
import vue from '@vitejs/plugin-vue';

/* Device mode: no favicon files are shipped (the theme store paints a themed
 * data-URL favicon at runtime). An empty data: icon also stops the browser
 * from spending one of the device's few connections on /favicon.ico. */
function deviceFavicon(): Plugin {
  return {
    name: 'nyabula-device-favicon',
    transformIndexHtml: {
      order: 'pre',
      handler: (html) => {
        let first = true;
        return html.replace(/^[ \t]*<link rel="icon"[^>]*>\r?\n/gm, (line) => {
          if (!first) return '';
          first = false;
          return line.replace(/<link[^>]*>/, '<link rel="icon" href="data:," />');
        });
      },
    },
  };
}

export default defineConfig(({ mode }) => {
  /* `device` = the site is served by the device's own static server: plain
   * http, same port as NyaLink, file names <= 64 chars, few connections. */
  const device = mode === 'device';
  return {
    plugins: [vue(), ...(device ? [deviceFavicon()] : [])],
    define: { __NYA_DEVICE__: JSON.stringify(device) },
    ...(device
      ? {
          base: './',
          publicDir: false as const,
          build: {
            outDir: 'dist-device',
            cssCodeSplit: false,
            rollupOptions: {
              output: {
                inlineDynamicImports: true,
                entryFileNames: 'assets/app-[hash].js',
                chunkFileNames: 'assets/app-[hash].js',
                assetFileNames: (asset: { name?: string }) => (asset.name?.endsWith('.css') ? 'assets/app-[hash][extname]' : 'assets/[hash][extname]'),
              },
            },
          },
        }
      : {}),
    server: {
      port: 5180,
      proxy: {
        // Cloud backend (HTTP API + relay WebSocket) during dev.
        '/api': { target: 'http://localhost:8080', changeOrigin: true, ws: true },
      },
    },
  };
});
