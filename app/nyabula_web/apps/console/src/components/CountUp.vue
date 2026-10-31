<script setup lang="ts">
/* Number that rolls up from 0 on mount (ease-out expo). */
import { onMounted, ref, watch } from 'vue';

const props = withDefaults(defineProps<{ to: number; ms?: number; decimals?: number; format?: (n: number) => string; delay?: number }>(), { ms: 1400, decimals: 0, delay: 0 });
const shown = ref('0');
let raf = 0;

function run(from: number) {
  cancelAnimationFrame(raf);
  const t0 = performance.now() + props.delay;
  const tick = (t: number) => {
    const p = Math.max(0, Math.min(1, (t - t0) / props.ms));
    const e = p === 1 ? 1 : 1 - Math.pow(2, -10 * p);
    const v = from + (props.to - from) * e;
    shown.value = props.format ? props.format(v) : v.toLocaleString('zh-CN', { maximumFractionDigits: props.decimals, minimumFractionDigits: props.decimals });
    if (p < 1) raf = requestAnimationFrame(tick);
  };
  raf = requestAnimationFrame(tick);
}
onMounted(() => run(0));
watch(() => props.to, (_, old) => run(old ?? 0));
</script>

<template><span class="cu">{{ shown }}</span></template>

<style scoped>
.cu { font-variant-numeric: tabular-nums; }
</style>
