<script setup lang="ts">
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useFeatureStatus } from '../../../composables/useFeatureStatus';
interface CallStatus {simulated?:boolean;connected?:{name:string};hfp:{state:string;call:string;number?:string;codec:string;audio:boolean}}
const emit=defineEmits<{(e:'open'):void}>();
const {session,status,busy,error,readError,action}=useFeatureStatus<CallStatus>('bt.status');
const call=computed(()=>status.value?.hfp.call??'idle');
const linked=computed(()=>status.value?.hfp.state==='connected');
const active=computed(()=>call.value!=='idle');
const labels:Record<string,string>={incoming:'来电',active:'正在通话',dialing:'正在拨号',alerting:'等待接听',held:'通话保持'};
const canAct=computed(()=>session.connected&&session.isOwner&&!busy.value&&!status.value?.simulated);
</script>
<template>
 <div class="call-mini">
  <div class="call-status"><span class="call-symbol" :class="{active}"><UiIcon name="call" :size="23"/></span><div><strong>{{ active?(labels[call]??'通话中'):linked?'等待来电':'通话音频未连接' }}</strong><small>{{ active ? status?.hfp.number || '已连接的蓝牙设备' : linked ? status?.connected?.name || 'HFP 已连接' : '连接手机后，可在这里接听' }}</small></div></div>
  <div class="call-actions"><template v-if="call==='incoming'"><button class="answer" :disabled="!canAct" @click="action('bt.call.answer')">接听</button><button :disabled="!canAct" @click="action('bt.call.reject')">拒接</button></template><button v-else-if="active" :disabled="!canAct" @click="action('bt.call.hangup')">挂断</button><button v-else @click="emit('open')">{{ linked?'通话设置':'连接手机' }}<UiIcon name="chevron_right" :size="15"/></button></div>
  <p v-if="error||readError" role="alert">{{error||readError}}</p>
 </div>
</template>
<style scoped>
.call-mini { display:grid; gap:10px; }.call-status { display:flex; align-items:center; gap:12px; }.call-symbol { width:40px;height:40px;display:grid;place-items:center;border-radius:50%;background:var(--md-surface-container-high);color:var(--md-on-surface-variant); }.call-symbol.active { color:var(--md-primary);background:var(--md-secondary-container); }.call-status>div { display:grid;gap:4px;min-width:0; }strong { font-size:14px; }small { font-size:12px;color:var(--md-on-surface-variant);overflow:hidden;text-overflow:ellipsis;white-space:nowrap; }.call-actions { display:flex;gap:8px;justify-content:flex-end; }button { display:inline-flex;align-items:center;gap:4px;border:0;border-radius:20px;padding:8px 12px;background:var(--md-surface-container-high);color:var(--md-primary);font:600 12px var(--font-body);cursor:pointer; }button.answer { background:var(--md-primary);color:var(--md-on-primary); }button:disabled { opacity:.4;cursor:default; }p { margin:0;color:var(--md-error);font-size:12px; }
</style>
