<script setup lang="ts">
/* What the two eye panels show, streamed from the device (GET /eyes/stream)
 * for whoever has no panels to look at. The device does nothing for this
 * until the request is open, and stops when it closes: the stream runs only
 * while this card is switched on, mounted, and the page is visible. */
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { MdCard, MdButton } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { otaEndpoint } from '../../lib/deviceMaint';
import { MIRROR_FRAME, MirrorPage, MirrorParser } from '../../lib/eyeMirror';

const WANTED_SLOT = 'nyabula.eyeMirror.on';
const RETRY_MS = 3000;
/** Below this many pages per second per eye the link is the limit. */
const SLOW_FPS = 5;

type Phase = 'off' | 'connecting' | 'live' | 'waiting' | 'error';

const session = useSessionStore();
const wanted = ref(false);
const phase = ref<Phase>('off');
const message = ref('');
const half = ref(false);
const fps = ref(0);
const kbps = ref(0);
const canvases = [ref<HTMLCanvasElement | null>(null), ref<HTMLCanvasElement | null>(null)];
const fullscreenHost = ref<HTMLElement | null>(null);

/* Same rule as the firmware upload: the token in this browser belongs to the
 * device that served the page, and only there is /eyes/stream the same origin. */
const endpoint = computed(() => {
  const ota = otaEndpoint(session.deviceKey, location);
  return ota ? { url: ota.url.replace(/\/ota\/upload$/, '/eyes/stream'), tokenSlot: ota.tokenSlot } : null;
});
const available = computed(() => endpoint.value !== null);
const status = computed(() => {
  if (phase.value === 'live') return `${fps.value.toFixed(0)} 帧/秒 · ${kbps.value.toFixed(0)} KB/s${half.value ? ' · 流畅' : ' · 高清'}`;
  if (phase.value === 'connecting') return '正在连接…';
  if (phase.value === 'waiting') return message.value;
  if (phase.value === 'error') return message.value;
  return '未开启';
});

let abort: AbortController | null = null;
let retry: number | undefined;
let run = 0;

function token(): string | null {
  try {
    return endpoint.value ? localStorage.getItem(endpoint.value.tokenSlot) : null;
  } catch {
    return null;
  }
}

function paint(eye: number, page: MirrorPage): void {
  const canvas = canvases[eye]?.value;
  if (!canvas || page.width === 0) return;
  if (canvas.width !== page.width || canvas.height !== page.height) {
    canvas.width = page.width;
    canvas.height = page.height;
  }
  const context = canvas.getContext('2d');
  if (!context) return;
  const image = context.createImageData(page.width, page.height);
  image.data.set(page.rgba);
  context.putImageData(image, 0, 0);
}

function stop(): void {
  run++;
  window.clearTimeout(retry);
  abort?.abort();
  abort = null;
}

function later(text: string, kind: Phase = 'waiting'): void {
  phase.value = kind;
  message.value = text;
  const mine = run;
  retry = window.setTimeout(() => { if (mine === run) void start(); }, RETRY_MS);
}

async function start(): Promise<void> {
  stop();
  const mine = run;
  const ep = endpoint.value;
  const tk = token();
  if (!ep || !tk) {
    phase.value = 'error';
    message.value = '需要在设备自己的页面上登录后才能观看';
    return;
  }
  phase.value = 'connecting';
  abort = new AbortController();
  const pages = [new MirrorPage(), new MirrorPage()];
  const dirty = [false, false];
  let got = 0;
  let bytes = 0;
  let frames = 0;
  let since = performance.now();
  let slow = 0;
  try {
    const res = await fetch(`${ep.url}?scale=${half.value ? 2 : 1}`, {
      headers: { Authorization: `Bearer ${tk}` }, signal: abort.signal, cache: 'no-store',
    });
    if (mine !== run) return;
    if (res.status === 503) return later('已有两个页面在观看，稍后自动重试');
    if (res.status === 401) { phase.value = 'error'; message.value = '登录已失效，请重新登录'; return; }
    if (res.status === 404) { phase.value = 'error'; message.value = '设备固件还不支持屏幕镜像'; return; }
    if (!res.ok || !res.body) return later(`设备返回 ${res.status}`);
    const reader = res.body.getReader();
    const parser = new MirrorParser();
    const draw = (): void => {
      if (mine !== run) return;
      for (let eye = 0; eye < 2; eye++) if (dirty[eye]) { dirty[eye] = false; paint(eye, pages[eye]); }
      requestAnimationFrame(draw);
    };
    requestAnimationFrame(draw);
    for (;;) {
      const { done, value } = await reader.read();
      if (done || mine !== run) break;
      bytes += value.length;
      for (const record of parser.push(value)) {
        if (record.type !== MIRROR_FRAME || record.eye > 1) continue;
        pages[record.eye].apply(record);
        dirty[record.eye] = true;
        frames++;
        got++;
        phase.value = 'live';
      }
      const now = performance.now();
      if (now - since >= 1000) {
        fps.value = frames / 2 / ((now - since) / 1000);
        kbps.value = bytes / 1024 / ((now - since) / 1000);
        /* Full size over a slow link: pages arrive late rather than small.
         * Three slow seconds with something to show, and half size it is. */
        slow = !half.value && bytes > 150 * 1024 && fps.value < SLOW_FPS ? slow + 1 : 0;
        bytes = 0;
        frames = 0;
        since = now;
        if (slow >= 3) { half.value = true; return void start(); }
      }
    }
    if (mine !== run) return;
    /* The device ends a full-size stream it has no memory for before the
     * first page; half size needs a quarter of it. */
    if (got === 0 && !half.value) { half.value = true; return void start(); }
    later('画面中断，正在重连…');
  } catch (e) {
    if (mine !== run) return;
    later(e instanceof Error && e.name !== 'AbortError' ? '连接中断，正在重连…' : '正在重连…');
  }
}

function apply(): void {
  if (wanted.value && available.value && !document.hidden) void start();
  else { stop(); phase.value = 'off'; }
}
function toggle(): void {
  wanted.value = !wanted.value;
  try { localStorage.setItem(WANTED_SLOT, wanted.value ? '1' : '0'); } catch { /* private mode */ }
  apply();
}
function quality(): void {
  half.value = !half.value;
  apply();
}
function fullscreen(): void {
  void fullscreenHost.value?.requestFullscreen?.();
}

onMounted(() => {
  try { wanted.value = localStorage.getItem(WANTED_SLOT) === '1'; } catch { /* private mode */ }
  document.addEventListener('visibilitychange', apply);
  apply();
});
watch(available, apply);
onBeforeUnmount(() => {
  document.removeEventListener('visibilitychange', apply);
  stop();
});
</script>

<template>
  <MdCard title="屏幕实时画面">
    <p v-if="!available" class="hint">屏幕镜像只在设备自己的页面上可用（浏览器直接打开设备地址）。</p>
    <template v-else>
      <div ref="fullscreenHost" class="eyes" :class="{ idle: phase !== 'live' }">
        <canvas :ref="canvases[0]" width="360" height="360" aria-label="左眼屏幕" />
        <canvas :ref="canvases[1]" width="360" height="360" aria-label="右眼屏幕" />
      </div>
      <div class="bar">
        <span class="state" :class="phase" role="status">{{ status }}</span>
        <span class="grow" />
        <MdButton v-if="wanted" variant="text" @click="quality">{{ half ? '切到高清' : '切到流畅' }}</MdButton>
        <MdButton v-if="phase === 'live'" variant="text" @click="fullscreen">全屏</MdButton>
        <MdButton :variant="wanted ? 'tonal' : 'filled'" @click="toggle">{{ wanted ? '停止' : '开始观看' }}</MdButton>
      </div>
      <p class="hint">两块眼睛屏幕上正在显示的内容，给手边没有屏幕的人看。只有这里开着时设备才会传画面，关掉或离开页面就停。</p>
    </template>
  </MdCard>
</template>

<style scoped>
.eyes { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; padding: 16px; border-radius: 16px; background: #000; }
.eyes canvas { width: 100%; max-width: 360px; aspect-ratio: 1; justify-self: center; border-radius: 50%; background: #050505; box-shadow: 0 0 0 2px #1d1d1d; }
.eyes.idle canvas { opacity: .35; }
.eyes:fullscreen { align-content: center; gap: 6vw; padding: 6vw; }
.eyes:fullscreen canvas { max-width: min(42vw, 84vh); }
.bar { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; margin-top: 12px; }
.grow { flex: 1; }
.state { font-size: 13px; color: var(--md-sys-color-on-surface-variant); font-variant-numeric: tabular-nums; }
.state.live { color: var(--md-sys-color-primary); }
.state.error { color: var(--md-sys-color-error); }
.hint { margin: 10px 0 0; font-size: 12.5px; line-height: 1.6; color: var(--md-sys-color-on-surface-variant); }
</style>
