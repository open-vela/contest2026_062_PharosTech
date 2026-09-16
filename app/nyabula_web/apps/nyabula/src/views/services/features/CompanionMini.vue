<script setup lang="ts">
import { computed, onMounted } from 'vue';
import { MdButton, MdSwitch } from '@nyabula/ui';
import { useCompanionStore } from '../../../stores/companion';
import type { FormFactor } from '../../../composables/useFormFactor';
defineProps<{ type:string; ff:FormFactor; active:boolean; payload:Record<string,unknown>|null }>();
const companion=useCompanionStore();
onMounted(() => void companion.refresh());
const label=computed(() => !companion.state?.enabled ? '主动陪伴已关闭'
  : companion.state.quiet_now ? '免打扰中'
  : `今日 ${companion.state.daily_count}/${companion.state.daily_limit} 次`);
function toggle(value:boolean):void {
  const state=companion.state;
  if (!state) return;
  void companion.act('configure', { enabled:value, mode:state.mode, quiet_start:state.quiet_start,
    quiet_end:state.quiet_end, utc_offset_minutes:state.utc_offset_minutes,
    minimum_interval_minutes:state.minimum_interval_minutes, daily_limit:state.daily_limit });
}
</script>
<template><div class="companion-mini">
  <MdSwitch :model-value="companion.state?.enabled ?? false" :disabled="!companion.available" @update:model-value="toggle" />
  <span>{{ label }}</span>
  <MdButton variant="tonal" :disabled="!companion.state?.enabled || companion.state?.quiet_now || companion.busy" @click="companion.act('run', {})">问候</MdButton>
</div></template>
<style scoped>.companion-mini { display:flex; align-items:center; gap:10px; }.companion-mini span { flex:1; }</style>
