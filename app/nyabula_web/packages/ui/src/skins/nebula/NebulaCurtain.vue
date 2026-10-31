<script setup lang="ts">
/* Nebula route curtain · "Iris". A lit disc blooms out of the tapped dock
 * item (nebulaRouteIntent; screen centre if unknown) until it covers the
 * screen and announces the destination: 正在前往 / page name / sliding bar /
 * 载入中. When the page is ready the text leaves and the panel opens like a
 * pupil from the centre of the content, a bright rim riding the edge.
 * Contract (see ClassicCurtain): markCovered() once covered;
 * routeOverlayVisible is cleared as the reveal starts. */
import { computed, nextTick, ref, watch } from 'vue';
import { useLoadingStore } from '../../stores/loading';
import { nebulaRouteIntent as intent } from './routeIntent';

const loading = useLoadingStore();
type Stage = 'idle' | 'enter' | 'leave';
const stage = ref<Stage>('idle');
const panel = ref<HTMLElement | null>(null);
const rim = ref<HTMLElement | null>(null);
const hole = ref({ x: 0, y: 0, R: 0 });
const label = ref('');
const ENTER_MS = 560;
const LEAVE_MS = 760;
const far = (x: number, y: number) => Math.hypot(Math.max(x, innerWidth - x), Math.max(y, innerHeight - y));
let timer = 0;

watch(
  () => loading.routeLoading,
  async (on) => {
    window.clearTimeout(timer);
    if (on) {
      label.value = intent.label || '下一页';
      stage.value = 'enter';
      await nextTick();
      const x = intent.x >= 0 ? intent.x : innerWidth / 2;
      const y = intent.y >= 0 ? intent.y : innerHeight / 2;
      panel.value?.animate([{ clipPath: `circle(0px at ${x}px ${y}px)` }, { clipPath: `circle(${far(x, y)}px at ${x}px ${y}px)` }], {
        duration: ENTER_MS,
        easing: 'cubic-bezier(0.7, 0, 0.25, 1)',
        fill: 'forwards',
      });
      timer = window.setTimeout(() => stage.value === 'enter' && loading.markCovered(), ENTER_MS);
    } else if (stage.value === 'enter') {
      loading.routeOverlayVisible = false;
      const main = document.querySelector<HTMLElement>('[data-nb-main]')?.getBoundingClientRect();
      const x = main ? main.left + main.width / 2 : innerWidth / 2;
      const y = main ? main.top + main.height / 2 : innerHeight / 2;
      hole.value = { x, y, R: far(x, y) + 60 };
      stage.value = 'leave';
      await nextTick();
      const opts: KeyframeAnimationOptions = { duration: LEAVE_MS, easing: 'cubic-bezier(0.55, 0, 0.2, 1)', fill: 'forwards' };
      panel.value?.animate([{ '--nb-hole': '0px' }, { '--nb-hole': `${hole.value.R}px` }], opts);
      rim.value?.animate(
        [
          { transform: 'translate(-50%, -50%) scale(0)', opacity: 1 },
          { transform: 'translate(-50%, -50%) scale(1)', opacity: 0.15 },
        ],
        opts,
      );
      timer = window.setTimeout(() => stage.value === 'leave' && (stage.value = 'idle'), LEAVE_MS + 40);
    }
  },
);
const visible = computed(() => stage.value !== 'idle');
</script>

<template>
  <Teleport to="body">
    <div v-if="visible" class="nb-curtain" :class="stage">
      <div ref="panel" class="panel" :style="{ '--hx': hole.x + 'px', '--hy': hole.y + 'px' }">
        <div class="mid">
          <p class="going">正在前往</p>
          <p class="name">{{ label }}</p>
          <div class="bar"><i /></div>
          <p class="loading">载入中</p>
        </div>
      </div>
      <span v-if="stage === 'leave'" ref="rim" class="rim" :style="{ left: hole.x + 'px', top: hole.y + 'px', width: hole.R * 2 + 'px', height: hole.R * 2 + 'px' }" />
    </div>
  </Teleport>
</template>

<style scoped>
.nb-curtain { position: fixed; inset: 0; z-index: 9000; pointer-events: auto; }
.nb-curtain.leave { pointer-events: none; }
.panel {
  position: absolute;
  inset: 0;
  container-type: size;
  overflow: hidden;
  color: #fff;
  clip-path: circle(0px at 50% 100%);
  --nb-hole: 0px;
  background:
    radial-gradient(80% 60% at 50% 115%, rgba(var(--md-primary-rgb), 0.62), transparent 70%),
    radial-gradient(70% 55% at 85% -10%, rgba(139, 124, 255, 0.4), transparent 70%),
    linear-gradient(180deg, color-mix(in srgb, rgb(var(--md-primary-rgb)) 16%, #0b0f26), #070918);
}
:root[data-mode='light'] .panel {
  background:
    radial-gradient(80% 60% at 50% 115%, rgba(255, 255, 255, 0.28), transparent 70%),
    linear-gradient(160deg, var(--md-primary-deep), rgb(var(--md-primary-rgb)));
}
.leave .panel {
  -webkit-mask-image: radial-gradient(circle at var(--hx) var(--hy), transparent var(--nb-hole), #000 calc(var(--nb-hole) + 48px));
  mask-image: radial-gradient(circle at var(--hx) var(--hy), transparent var(--nb-hole), #000 calc(var(--nb-hole) + 48px));
}
.mid { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; }
.going { margin: 0; font: 600 1.8cqmin var(--font-body); letter-spacing: 0.5em; padding-left: 0.5em; opacity: 0.7; }
.name { margin: 1.6cqmin 0 3.6cqmin; font: 400 8.5cqmin/1.05 var(--font-title); letter-spacing: 0.04em; text-shadow: 0 0 4cqmin rgba(var(--md-primary-rgb), 0.45); }
.bar { position: relative; width: 26cqmin; height: 3px; border-radius: 3px; background: rgba(255, 255, 255, 0.16); overflow: hidden; }
.bar i { position: absolute; top: 0; bottom: 0; left: 0; width: 38%; border-radius: inherit; background: linear-gradient(90deg, transparent, #fff 55%, #fff); box-shadow: 0 0 10px rgba(255, 255, 255, 0.8); animation: nb-slide 1.15s cubic-bezier(0.65, 0, 0.35, 1) infinite; }
@keyframes nb-slide { from { transform: translateX(-110%); } to { transform: translateX(280%); } }
.loading { margin: 2.4cqmin 0 0; font: 600 1.6cqmin var(--font-body); letter-spacing: 0.45em; padding-left: 0.45em; opacity: 0.55; }
.mid > * { opacity: 0; }
.enter .going { animation: nb-rise 0.5s cubic-bezier(0.22, 1, 0.36, 1) 0.24s both; }
.enter .name { animation: nb-rise 0.6s cubic-bezier(0.22, 1, 0.36, 1) 0.3s both; }
.enter .bar { animation: nb-grow 0.55s cubic-bezier(0.22, 1, 0.36, 1) 0.4s both; }
.enter .loading { animation: nb-rise 0.5s cubic-bezier(0.22, 1, 0.36, 1) 0.46s both; }
@keyframes nb-rise { from { opacity: 0; transform: translateY(1.4cqmin); filter: blur(4px); } to { opacity: 1; } }
@keyframes nb-grow { from { opacity: 0; transform: scaleX(0.3); } to { opacity: 1; } }
.leave .going { opacity: 0.7; }
.leave .name, .leave .bar, .leave .loading { opacity: 1; }
.leave .mid { animation: nb-leave 0.3s ease forwards; }
@keyframes nb-leave { to { opacity: 0; transform: scale(0.96); } }
.rim {
  position: absolute;
  border-radius: 50%;
  pointer-events: none;
  transform: translate(-50%, -50%) scale(0);
  box-shadow: 0 0 0 2px rgba(255, 255, 255, 0.9), 0 0 24px 6px rgba(var(--md-primary-rgb), 0.8), inset 0 0 24px 6px rgba(var(--md-primary-rgb), 0.6);
}
</style>
