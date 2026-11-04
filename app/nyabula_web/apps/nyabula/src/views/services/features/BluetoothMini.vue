<script setup lang="ts">
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useFeatureStatus } from '../../../composables/useFeatureStatus';
interface Bluetooth { state:string; discoverable:boolean; simulated?:boolean; connected?:{name:string;address:string}; connections?:{connected:boolean}[]; avrcp:{connected:boolean;playing:boolean;pending?:boolean;controlError?:number;title?:string;artist?:string} }
const { session, status, busy, error, readError, action } = useFeatureStatus<Bluetooth>('bt.status');
const canControl = computed(() => session.connected && session.isOwner && !busy.value && !status.value?.simulated && status.value?.avrcp.connected && !status.value.avrcp.pending);
function media(key:string) { if(canControl.value && status.value?.connected) void action(`bt.media.${key}`,{address:status.value.connected.address}); }
</script>
<template>
  <div class="bt-mini">
    <div class="track"><strong>{{ status?.avrcp.title || status?.connected?.name || (session.connected ? '等待设备连接' : '设备离线') }}</strong><span>{{ status?.connected ? `${status.connected.name} · ${status.connections?.filter(c=>c.connected).length || 1} 台在线` : '连接手机或电脑，播放你的音乐' }}</span></div>
    <div class="controls" v-if="status?.connected"><button aria-label="上一首" :disabled="!canControl" @click="media('prev')"><UiIcon name="skip_previous" :size="19"/></button><button class="primary" :aria-label="status.avrcp.playing ? '暂停' : '播放'" :disabled="!canControl" @click="media(status.avrcp.playing ? 'pause' : 'play')"><UiIcon :name="status.avrcp.playing ? 'pause' : 'play_arrow'" :size="22"/></button><button aria-label="下一首" :disabled="!canControl" @click="media('next')"><UiIcon name="skip_next" :size="19"/></button></div>
    <button v-else class="pair" :disabled="!session.isOwner || !session.connected || busy || status?.state !== 'ready' || status?.simulated" @click="action('bt.discoverable',{enabled:!status?.discoverable,seconds:120})">{{ status?.discoverable ? '关闭配对窗口' : '允许新设备配对' }}</button>
    <p v-if="error || readError || status?.avrcp.controlError" role="alert">{{ error || readError || `播放控制失败（${status?.avrcp.controlError}）` }}</p>
  </div>
</template>
<style scoped>
.bt-mini { display:flex; align-items:center; gap:12px; flex-wrap:wrap; }
.track { flex:1; min-width:100px; display:grid; gap:5px; }
.track strong { font-size:15px; overflow:hidden; white-space:nowrap; text-overflow:ellipsis; }
.track span { color:var(--md-on-surface-variant); font-size:12px; }
.controls { display:flex; gap:5px; }
button { border:0; color:var(--md-on-surface); background:var(--md-surface-container-high); cursor:pointer; }
.controls button { width:36px; height:36px; border-radius:50%; display:grid; place-items:center; padding:0; }
.controls .primary { color:var(--md-on-primary); background:var(--md-primary); }
.pair { border-radius:22px; padding:10px 14px; font:600 12px var(--font-body); color:var(--md-primary); }
button:disabled { opacity:.4; cursor:default; }
p { flex-basis:100%; margin:0; color:var(--md-error); font-size:12px; }
</style>
