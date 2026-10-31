<script setup lang="ts">
/* Device constellation: the Cloud relay is the sun, the account's devices
 * orbit it. Online devices (GET /devices `online`) get a lit link with
 * packets flowing to the core; offline ones sit dim. Click opens detail. */
import { computed } from 'vue';
import type { Device } from '../../api/client';

const props = defineProps<{ devices: Device[] }>();
const emit = defineEmits<{ (e: 'pick', id: string): void }>();
const W = 460;
const H = 300;
const cx = W / 2;
const cy = H / 2;
const MAX = 12;
const nodes = computed(() => {
  const list = props.devices.slice(0, MAX);
  return list.map((d, i) => {
    const a = -Math.PI / 2 + (i / Math.max(1, list.length)) * Math.PI * 2 + 0.35;
    const rx = 170 - (i % 2) * 36;
    const ry = 108 - (i % 2) * 22;
    const label = d.name || d.deviceId;
    return { ...d, label: label.length > 10 ? label.slice(0, 9) + '…' : label, x: cx + Math.cos(a) * rx, y: cy + Math.sin(a) * ry, delay: i * 0.4 };
  });
});
</script>

<template>
  <svg class="map" :viewBox="`0 0 ${W} ${H}`" role="img" :aria-label="`${devices.length} 台设备`">
    <defs>
      <radialGradient id="nbcCore"><stop offset="0" class="s-core" /><stop offset="0.35" class="s-acc" /><stop offset="1" class="s-acc" stop-opacity="0" /></radialGradient>
      <radialGradient id="nbcNode"><stop offset="0" class="s-core" /><stop offset="0.4" class="s-acc2" /><stop offset="1" class="s-acc2" stop-opacity="0" /></radialGradient>
    </defs>
    <ellipse :cx="cx" :cy="cy" rx="170" ry="108" class="orbit" />
    <ellipse :cx="cx" :cy="cy" rx="134" ry="86" class="orbit o2" />
    <g v-for="n in nodes" :key="n.deviceId">
      <line :x1="cx" :y1="cy" :x2="n.x" :y2="n.y" class="link" :class="{ on: n.online }" />
      <line v-if="n.online" :x1="n.x" :y1="n.y" :x2="cx" :y2="cy" class="packet" :style="{ animationDelay: n.delay + 's' }" />
    </g>
    <circle :cx="cx" :cy="cy" r="46" fill="url(#nbcCore)" class="core-glow" />
    <circle :cx="cx" :cy="cy" r="15" class="core" />
    <text :x="cx" :y="cy + 40" class="core-lbl" text-anchor="middle">Nyabula Cloud</text>
    <g
      v-for="n in nodes"
      :key="'n' + n.deviceId"
      class="node"
      :class="{ off: !n.online }"
      :style="{ animationDelay: n.delay + 's' }"
      tabindex="0"
      role="button"
      :aria-label="`${n.label} ${n.online ? '在线' : '离线'}`"
      @click="emit('pick', n.deviceId)"
      @keydown.enter="emit('pick', n.deviceId)"
    >
      <circle v-if="n.online" :cx="n.x" :cy="n.y" r="10" class="ping" :style="{ animationDelay: n.delay + 's' }" />
      <circle :cx="n.x" :cy="n.y" r="16" :fill="n.online ? 'url(#nbcNode)' : undefined" :class="{ ghost: !n.online }" />
      <circle :cx="n.x" :cy="n.y" r="5" class="pip" />
      <text :x="n.x" :y="n.y + 30" text-anchor="middle" class="n-lbl">{{ n.label }}</text>
    </g>
    <text v-if="!devices.length" :x="cx" :y="cy - 64" text-anchor="middle" class="n-lbl">还没有设备绕着它转</text>
  </svg>
</template>

<style scoped>
.map { width: 100%; height: auto; display: block; overflow: visible; }
.s-core { stop-color: var(--md-on-surface); }
.s-acc { stop-color: var(--md-primary); }
.s-acc2 { stop-color: var(--md-tertiary); }
.orbit { fill: none; stroke: var(--line); stroke-dasharray: 2 6; }
.o2 { stroke: color-mix(in srgb, var(--md-tertiary) 20%, transparent); }
.link { stroke: var(--line); stroke-width: 1; }
.link.on { stroke: rgba(var(--acc-rgb), 0.3); }
.packet { stroke: var(--md-primary); stroke-width: 2.5; stroke-linecap: round; stroke-dasharray: 6 400; stroke-dashoffset: 0; filter: drop-shadow(0 0 4px var(--md-primary)); animation: flow 2.4s linear infinite; }
@keyframes flow { from { stroke-dashoffset: 0; } to { stroke-dashoffset: -406; } }
.core { fill: color-mix(in srgb, var(--md-primary) 25%, #ffffff); }
.core-glow { transform-box: fill-box; transform-origin: center; animation: breathe 4s ease-in-out infinite; }
@keyframes breathe { 50% { transform: scale(1.15); opacity: 0.8; } }
.core-lbl { fill: var(--md-primary); font-size: 11px; font-family: var(--font-body); letter-spacing: 0.1em; }
.node { cursor: pointer; outline: none; animation: bob 5s ease-in-out infinite; }
.node:hover .pip, .node:focus-visible .pip { fill: var(--md-primary); }
@keyframes bob { 50% { transform: translateY(-4px); } }
.ghost { fill: var(--wash-2); }
.pip { fill: var(--md-on-surface); transition: fill 0.2s; }
.off .pip { fill: var(--dim); }
.ping { fill: none; stroke: var(--md-tertiary); stroke-width: 1.5; transform-box: fill-box; transform-origin: center; animation: ping 2.6s ease-out infinite; }
@keyframes ping { from { transform: scale(1); opacity: 0.9; } to { transform: scale(3.2); opacity: 0; } }
.n-lbl { fill: var(--sub); font-size: 11px; font-family: var(--font-body); }
.off .n-lbl { fill: var(--dim); }
</style>
