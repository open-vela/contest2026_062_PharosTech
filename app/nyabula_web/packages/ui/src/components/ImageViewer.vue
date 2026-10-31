<script setup lang="ts">
/* Fullscreen lightbox (ported from Myself): wheel / pinch zoom, drag to pan,
 * FLIP fly-in from the origin rect, arrow keys for multi-image, double tap on
 * the backdrop or the close button to exit. */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import UiIcon from './UiIcon.vue';

export interface OriginRect {
  left: number;
  top: number;
  width: number;
  height: number;
}

const props = defineProps<{
  images: string[];
  startIndex?: number;
  originRect?: OriginRect;
  captions?: string[];
}>();
const emit = defineEmits<{ close: [] }>();

const index = ref(props.startIndex ?? 0);
const scale = ref(1);
const tx = ref(0);
const ty = ref(0);
const closing = ref(false);
const dragging = ref(false);
const flying = ref(false);
const imgReady = ref(!props.originRect);
const imgEl = ref<HTMLImageElement | null>(null);

function flyTransformFromOrigin(): { tx: number; ty: number; s: number } | null {
  const rect = props.originRect;
  const el = imgEl.value;
  if (!rect || !el) return null;
  const display = el.getBoundingClientRect();
  const baseW = display.width / scale.value;
  const baseH = display.height / scale.value;
  return {
    tx: rect.left + rect.width / 2 - window.innerWidth / 2,
    ty: rect.top + rect.height / 2 - window.innerHeight / 2,
    s: Math.max(rect.width / baseW, rect.height / baseH),
  };
}

function onImgLoad(): void {
  if (!props.originRect || imgReady.value) {
    imgReady.value = true;
    return;
  }
  const from = flyTransformFromOrigin();
  if (from) {
    tx.value = from.tx;
    ty.value = from.ty;
    scale.value = from.s;
  }
  imgReady.value = true;
  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      flying.value = true;
      tx.value = 0;
      ty.value = 0;
      scale.value = 1;
      window.setTimeout(() => (flying.value = false), 520);
    });
  });
}

const isTouch = 'ontouchstart' in window;
const guide = computed(() => [isTouch ? '双指捏合缩放' : '滚轮缩放', '拖动平移', isTouch ? '双击空白退出' : '双击空白或 Esc 退出']);
const imgStyle = computed(() => ({ transform: `translate(${tx.value}px, ${ty.value}px) scale(${scale.value})` }));

function resetTransform(): void {
  scale.value = 1;
  tx.value = 0;
  ty.value = 0;
}
function clampScale(v: number): number {
  return Math.min(8, Math.max(0.5, v));
}

function requestClose(): void {
  if (closing.value) return;
  closing.value = true;
  if (props.originRect && index.value === (props.startIndex ?? 0)) {
    const to = flyTransformFromOrigin();
    if (to) {
      flying.value = true;
      tx.value = to.tx;
      ty.value = to.ty;
      scale.value = to.s;
    }
  }
  window.setTimeout(() => emit('close'), 420);
}

let lastTap = 0;
function onBackdropTap(e: MouseEvent | PointerEvent): void {
  if (e.target !== e.currentTarget) return;
  const now = performance.now();
  if (now - lastTap < 320) requestClose();
  lastTap = now;
}

function onWheel(e: WheelEvent): void {
  e.preventDefault();
  const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
  const next = clampScale(scale.value * factor);
  const ratio = next / scale.value;
  const cx = e.clientX - window.innerWidth / 2;
  const cy = e.clientY - window.innerHeight / 2;
  tx.value = cx - (cx - tx.value) * ratio;
  ty.value = cy - (cy - ty.value) * ratio;
  scale.value = next;
}

const pointers = new Map<number, { x: number; y: number }>();
let panReady = false;
let last = { x: 0, y: 0 };
let pinchDist = 0;

function onPointerDown(e: PointerEvent): void {
  (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
  pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (pointers.size === 1) {
    last = { x: e.clientX, y: e.clientY };
    panReady = true;
    dragging.value = true;
  } else if (pointers.size === 2) {
    panReady = false;
    dragging.value = false;
    const [a, b] = [...pointers.values()];
    pinchDist = Math.hypot(a.x - b.x, a.y - b.y);
  }
}
function onPointerMove(e: PointerEvent): void {
  if (!pointers.has(e.pointerId)) return;
  pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  if (pointers.size === 2) {
    const [a, b] = [...pointers.values()];
    const dist = Math.hypot(a.x - b.x, a.y - b.y);
    if (pinchDist > 0) {
      const next = clampScale(scale.value * (dist / pinchDist));
      const ratio = next / scale.value;
      const cx = (a.x + b.x) / 2 - window.innerWidth / 2;
      const cy = (a.y + b.y) / 2 - window.innerHeight / 2;
      tx.value = cx - (cx - tx.value) * ratio;
      ty.value = cy - (cy - ty.value) * ratio;
      scale.value = next;
    }
    pinchDist = dist;
    return;
  }
  if (panReady) {
    tx.value += e.clientX - last.x;
    ty.value += e.clientY - last.y;
  }
  last = { x: e.clientX, y: e.clientY };
}
function onPointerUp(e: PointerEvent): void {
  pointers.delete(e.pointerId);
  if (pointers.size < 2) pinchDist = 0;
  if (pointers.size === 0) {
    panReady = false;
    dragging.value = false;
  }
}

function go(delta: number): void {
  const len = props.images.length;
  index.value = (index.value + delta + len) % len;
  resetTransform();
}
function onKey(e: KeyboardEvent): void {
  if (e.key === 'Escape') requestClose();
  if (e.key === 'ArrowLeft') go(-1);
  if (e.key === 'ArrowRight') go(1);
}

onMounted(() => {
  window.addEventListener('keydown', onKey);
  document.documentElement.style.overflow = 'hidden';
});
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKey);
  document.documentElement.style.overflow = '';
});
</script>

<template>
  <Teleport to="body">
    <div class="viewer" :class="{ closing }" @click="onBackdropTap" @wheel.prevent="onWheel">
      <div class="stage">
        <img
          :key="index"
          ref="imgEl"
          class="stage-img"
          :class="{ dragging, flying, flip: !!originRect, ready: imgReady }"
          :src="images[index]"
          alt=""
          draggable="false"
          :style="imgStyle"
          @load="onImgLoad"
          @pointerdown.prevent="onPointerDown"
          @pointermove="onPointerMove"
          @pointerup="onPointerUp"
          @pointercancel="onPointerUp"
        />
      </div>
      <button class="close" aria-label="关闭" @click="requestClose"><UiIcon name="close" :size="22" /></button>
      <template v-if="images.length > 1">
        <button class="nav prev" aria-label="上一张" @click.stop="go(-1)"><UiIcon name="chevron_left" :size="26" /></button>
        <button class="nav next" aria-label="下一张" @click.stop="go(1)"><UiIcon name="chevron_right" :size="26" /></button>
        <span class="counter">{{ index + 1 }} / {{ images.length }}</span>
      </template>
      <p v-if="captions?.[index]" class="caption">{{ captions[index] }}</p>
      <ul class="guide">
        <li v-for="(line, i) in guide" :key="line" :style="{ '--i': i }">{{ line }}</li>
      </ul>
    </div>
  </Teleport>
</template>

<style scoped>
.viewer {
  position: fixed;
  inset: 0;
  z-index: 9500;
  user-select: none;
  background: rgba(8, 8, 12, 0.82);
  backdrop-filter: blur(18px);
  -webkit-backdrop-filter: blur(18px);
  overflow: hidden;
  touch-action: none;
  animation: viewer-in 0.35s var(--ease-out) both;
}
.viewer.closing { animation: viewer-out 0.4s var(--ease-out) both; }
@keyframes viewer-in { from { opacity: 0; } }
@keyframes viewer-out { to { opacity: 0; } }
.stage { position: absolute; inset: 0; display: grid; place-items: center; pointer-events: none; }
.stage-img {
  max-width: 88vw;
  max-height: 86vh;
  pointer-events: auto;
  cursor: grab;
  will-change: transform;
  transition: transform 0.08s linear;
  animation: img-in 0.45s var(--ease-out) both;
  border-radius: 6px;
}
.stage-img.dragging { cursor: grabbing; transition: none; }
.stage-img.flip { animation: none; opacity: 0; }
.stage-img.flip.ready { opacity: 1; }
.stage-img.flying { transition: transform 0.5s var(--ease-out); }
@keyframes img-in { from { opacity: 0; scale: 0.9; } }
.viewer.closing .stage-img:not(.flip) { animation: img-out 0.35s var(--ease-out) both; }
@keyframes img-out { to { opacity: 0; scale: 0.92; } }
.close,
.nav {
  position: absolute;
  width: 44px;
  height: 44px;
  border-radius: 50%;
  border: 1px solid rgba(255, 255, 255, 0.25);
  background: rgba(255, 255, 255, 0.08);
  color: #fff;
  display: grid;
  place-items: center;
  cursor: pointer;
  transition: transform var(--dur-fast) var(--ease-spring), background var(--dur-fast);
}
.close { top: 22px; right: 22px; }
.close:hover { transform: scale(1.12) rotate(90deg); background: rgba(var(--md-primary-rgb), 0.35); }
.nav { top: 50%; transform: translateY(-50%); }
.nav.prev { left: 20px; }
.nav.next { right: 20px; }
.nav:hover { transform: translateY(-50%) scale(1.12); background: rgba(var(--md-primary-rgb), 0.35); }
.counter {
  position: absolute;
  top: 30px;
  left: 50%;
  transform: translateX(-50%);
  color: rgba(255, 255, 255, 0.85);
  font-size: 14px;
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.1em;
}
.caption {
  position: absolute;
  bottom: 24px;
  left: 50%;
  transform: translateX(-50%);
  margin: 0;
  color: rgba(255, 255, 255, 0.85);
  font-size: 14px;
  padding: 6px 14px;
  background: rgba(0, 0, 0, 0.35);
  border-radius: 999px;
}
.guide {
  position: absolute;
  left: 24px;
  bottom: 24px;
  list-style: none;
  margin: 0;
  padding: 0;
  display: flex;
  flex-direction: column;
  gap: 8px;
  pointer-events: none;
}
.guide li {
  color: rgba(255, 255, 255, 0.78);
  font-size: 13px;
  padding: 7px 14px;
  background: rgba(255, 255, 255, 0.08);
  border: 1px solid rgba(255, 255, 255, 0.14);
  border-radius: 999px;
  backdrop-filter: blur(8px);
  width: fit-content;
  animation: guide-in 0.5s var(--ease-out) both;
  animation-delay: calc(0.25s + var(--i) * 0.12s);
}
@keyframes guide-in { from { opacity: 0; transform: translateX(-28px); filter: blur(6px); } to { opacity: 1; transform: none; filter: blur(0); } }
.viewer.closing .guide li { animation: guide-out 0.3s var(--ease-out) both; animation-delay: calc(var(--i) * 0.06s); }
@keyframes guide-out { to { opacity: 0; transform: translateX(-28px); filter: blur(6px); } }
@media (max-width: 768px) {
  .nav { display: none; }
  .guide { left: 14px; bottom: 18px; }
  .caption { bottom: auto; top: 70px; }
}
</style>
