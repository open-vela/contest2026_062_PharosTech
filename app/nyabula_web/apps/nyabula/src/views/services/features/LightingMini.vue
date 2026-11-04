<script setup lang="ts">
import { UiIcon } from '@nyabula/ui';
import { useLightingStore } from '../../../stores/lighting';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { LIGHT_ZONES } from '../../../lib/lighting';
const lights = useLightingStore();
defineEmits<{(e:'open'):void}>();
const on = (id: string) => lights.state?.groups.find(group => group.id === id)?.enabled === true;
useVisiblePoll(() => void lights.refresh(), 1500);
</script>
<template>
 <div class="lighting-mini">
  <div class="mini-zones"><button v-for="zone in LIGHT_ZONES" :key="zone.id" type="button" :class="{ on:on(zone.id) }" :aria-label="`${on(zone.id)?'关闭':'打开'}${zone.name}灯光`" :aria-pressed="on(zone.id)" :disabled="!lights.canWrite || lights.state?.sleeping" @click.stop="lights.set(zone.id, { enabled: !on(zone.id) })"><i/><span>{{ zone.name }}</span></button></div>
  <div class="mini-foot"><span>{{ lights.error || lights.readError || lights.label }}</span><button type="button" :disabled="!lights.canWrite || (!!lights.state?.sleeping && !lights.anyOn)" @click.stop="lights.set('all', { enabled: !lights.anyOn })"><UiIcon name="light_mode" :size="15"/>{{ lights.anyOn?'全关':'全开' }}</button></div>
 </div>
</template>
<style scoped>
.lighting-mini{display:grid;gap:10px}.mini-zones{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:5px}.mini-zones button{display:flex;align-items:center;justify-content:center;gap:5px;min-height:36px;padding:5px 3px;border:1px solid var(--md-outline-variant);border-radius:var(--radius-s);color:var(--md-on-surface-variant);background:transparent;font:500 12px var(--font-body);cursor:pointer}.mini-zones i{width:5px;height:5px;border-radius:50%;background:var(--md-outline)}.mini-zones button.on{background:var(--md-secondary-container);border-color:transparent;color:var(--md-on-secondary-container)}.mini-zones button.on i{background:var(--md-primary)}.mini-foot{display:flex;align-items:center;gap:8px;justify-content:space-between;font-size:12px;color:var(--md-on-surface-variant)}.mini-foot>span{min-width:0;overflow-wrap:anywhere}.mini-foot button{display:flex;align-items:center;gap:4px;flex:none;border:0;background:transparent;color:var(--md-primary);font:600 12px var(--font-body);min-height:32px;cursor:pointer}button:disabled{opacity:.45;cursor:default}button:focus-visible{outline:2px solid var(--md-primary);outline-offset:2px}
:global([data-skin='instrument'] .lighting-mini .mini-zones button){border-radius:2px}
</style>
