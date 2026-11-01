<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { MdButton, UiIcon, describeError } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useVisiblePoll } from '../../../composables/usePageVisible';
const props = defineProps<{ mode: 'media' | 'call' }>();
interface Connection {
  name: string; address: string; connected: boolean; selected: boolean;
  a2dpState: string; avrcpConnected: boolean; call: string;
}
interface Device { name: string; address: string; bonded: boolean; connected: boolean; connecting?: boolean; selected?: boolean }
interface Status {
  state: string; name: string; address: string; error: number;
  discoverable: boolean; bondsPersistent: boolean; simulated?: boolean;
  maxConnections?: number; connections?: Connection[];
  connected?: { name: string; address: string };
  pairing?: { name: string; address: string; secondsLeft: number; passkey?: string; displayOnly?: boolean };
  a2dp: { state: string; codec: string; sampleRate: number; channels: number };
  avrcp: { connected: boolean; playing: boolean; title?: string; artist?: string; pending?: boolean; controlError?: number };
  hfp: { state: string; call: string; number?: string; codec: string; audio: boolean };
  audio: { owner: string; error: number; mediaPackets: number; scoRxPackets: number; scoTxPackets: number; bufferMs: number };
}
const session = useSessionStore();
const status = ref<Status>();
const devices = ref<Device[]>([]);
const busy = ref(false);
const statusError = ref('');
const actionError = ref('');
let refreshing = false;
let epoch = 0;
const canEdit = computed(() => session.isOwner && session.connected && !busy.value && status.value?.state === 'ready' && !status.value?.simulated);
const connections = computed<Connection[]>(() => status.value?.connections ?? (status.value?.connected ? [{
  ...status.value.connected, connected: true, selected: true,
  a2dpState: status.value.a2dp.state, avrcpConnected: status.value.avrcp.connected, call: status.value.hfp.call,
}] : []));
const maxConnections = computed(() => status.value?.maxConnections ?? 1);
const canConnect = computed(() => canEdit.value && connections.value.length < maxConnections.value && connections.value.every(device => device.connected));
const canSelect = computed(() => canEdit.value && status.value?.hfp.call === 'idle' && !status.value.hfp.audio);
const canControl = computed(() => canEdit.value && status.value?.avrcp.connected && !status.value.avrcp.pending);
const controlError = computed(() => {
  const code = status.value?.avrcp.controlError;
  if (!code) return '';
  if (code === -110) return '未收到音源设备的控制响应，请重试。';
  if (code === -119 || code === -95) return '音源设备不支持这个媒体按键。';
  if (code === -13) return '音源设备拒绝了这个媒体按键。';
  return `播放控制失败（${code}），请重试。`;
});
const labels: Record<string, string> = { off:'已关闭', starting:'正在启动', ready:'已就绪', failed:'启动失败', idle:'无通话', incoming:'来电', dialing:'正在拨号', alerting:'等待接听', active:'通话中', held:'通话保持', disconnected:'未连接', connected:'已连接', open:'已连接', streaming:'正在播放' };
async function refresh() {
  if (!session.connected || refreshing || busy.value) return;
  refreshing = true;
  const revision = epoch;
  const client = session.client;
  try {
    const next = await session.request('bt.status') as unknown as Status;
    if (!next.hfp || !next.a2dp) throw Error('蓝牙状态格式无效');
    const remembered = session.isOwner ? await session.request('bt.devices') : { devices: [] };
    if (revision !== epoch || client !== session.client) return;
    status.value = next;
    devices.value = (remembered.devices ?? []) as Device[];
    statusError.value = '';
  } catch (error) {
    if (revision === epoch && client === session.client) statusError.value = describeError(error);
  } finally { refreshing = false; }
}
async function action(topic: string, data: Record<string, unknown> = {}) {
  if (busy.value) return;
  busy.value = true;
  actionError.value = '';
  const revision = ++epoch;
  const client = session.client;
  try {
    const next = await session.request(topic, data) as unknown as Status;
    if (revision === epoch && client === session.client && next?.a2dp && next?.hfp) status.value = next;
  } catch (error) {
    if (revision === epoch && client === session.client) actionError.value = describeError(error);
  } finally {
    if (revision === epoch) { busy.value = false; void refresh(); }
  }
}
function media(command: 'prev' | 'next' | 'play' | 'pause') {
  if (status.value?.connected) void action(`bt.media.${command}`, { address: status.value.connected.address });
}
watch(() => session.client, () => {
  epoch++; busy.value = false; status.value = undefined; devices.value = [];
  actionError.value = ''; statusError.value = ''; void refresh();
}, { immediate: true });
useVisiblePoll(() => void refresh(), 1000);
</script>
<template>
  <div class="bt-control">
    <div v-if="statusError" class="service-error" role="alert"><strong>暂时无法读取蓝牙状态</strong><span>{{ statusError }}</span><MdButton variant="text" @click="refresh">重试</MdButton></div>
    <div v-if="actionError" class="service-error" role="alert"><strong>操作未完成</strong><span>{{ actionError }}</span></div>
    <p v-if="status?.simulated" class="hint">模拟器没有蓝牙控制器；连接、配对和通话操作需要真实设备。</p>
    <template v-if="status">
      <section class="ui-panel control-card">
        <div class="section-head"><div><h3>{{status.name||'Nyabula 蓝牙'}}</h3><p>{{labels[status.state]??status.state}} · {{status.address||'控制器尚未就绪'}}</p></div><UiIcon name="bluetooth" :size="30"/></div>
        <div v-if="connections.length" class="connection-list">
          <div v-for="device in connections" :key="device.address" class="device-row" :class="{ selected: device.selected }">
            <span><strong>{{ device.name || device.address }}</strong><small>{{ device.connected ? (device.selected ? '当前音源' : '已连接 · 待命') : '正在连接' }} · {{ device.address }}</small></span>
            <div class="buttons">
              <MdButton v-if="!device.selected && status.maxConnections" variant="tonal" :disabled="!canSelect || !device.connected || device.a2dpState === 'disconnected'" @click="action('bt.select', { address: device.address })">切换音源</MdButton>
              <MdButton variant="text" :disabled="!canEdit" @click="action('bt.disconnect', { address: device.address })">断开</MdButton>
            </div>
          </div>
          <p v-if="maxConnections > 1" class="hint">最多连接 {{ maxConnections }} 台设备；播放控制只作用于当前音源。</p>
          <p v-if="status.hfp.call !== 'idle' || status.hfp.audio" class="hint">通话结束后可切换音源。</p>
        </div>
        <p v-else-if="!status.simulated" class="hint">在手机或电脑的蓝牙设置中连接本设备。</p>
        <div class="buttons"><MdButton :disabled="!canEdit" @click="action('bt.discoverable',{enabled:!status.discoverable,seconds:120})">{{status.discoverable?'关闭配对窗口':'允许设备配对 · 120 秒'}}</MdButton></div>
        <div v-if="status.pairing" class="pairing"><strong>{{status.pairing.name||status.pairing.address}} 请求配对</strong><p v-if="status.pairing.passkey" class="pairing-code">{{status.pairing.passkey}}</p><p>{{status.pairing.displayOnly ? '在另一台设备输入此数字' : status.pairing.passkey ? '确认两端显示的数字一致后再配对' : '请确认这是你正在连接的设备'}} · 剩余 {{status.pairing.secondsLeft}} 秒</p><div class="buttons"><MdButton variant="outlined" :disabled="!canEdit" @click="action('bt.pair.confirm',{accept:false,address:status.pairing.address})">拒绝</MdButton><MdButton v-if="!status.pairing.displayOnly" :disabled="!canEdit" @click="action('bt.pair.confirm',{accept:true,address:status.pairing.address})">确认配对</MdButton></div></div>
        <p v-if="!status.bondsPersistent && !status.simulated" class="hint">当前固件未启用配对信息持久化。</p>
      </section>
      <section class="ui-panel control-card" v-if="mode==='media'">
        <div class="section-head"><h3>蓝牙音乐</h3><span class="tag">{{labels[status.a2dp.state]??status.a2dp.state}}</span></div>
        <div class="track"><strong>{{status.avrcp.title||'等待蓝牙设备播放'}}</strong><span>{{status.avrcp.artist||'SBC · A2DP / AVRCP'}}</span></div>
        <p v-if="status.connected" class="hint">来自 {{ status.connected.name || status.connected.address }}</p>
        <div class="buttons"><MdButton variant="tonal" :disabled="!canControl" @click="media('prev')">上一首</MdButton><MdButton :disabled="!canControl" @click="media(status.avrcp.playing ? 'pause' : 'play')">{{ status.avrcp.pending ? '正在发送…' : status.avrcp.playing ? '暂停' : '播放' }}</MdButton><MdButton variant="tonal" :disabled="!canControl" @click="media('next')">下一首</MdButton></div>
        <p v-if="controlError" class="service-error" role="alert">{{ controlError }}</p>
        <p class="hint">{{status.a2dp.sampleRate||'—'}} Hz · {{status.a2dp.channels||'—'}} 声道 · 缓冲 {{status.audio.bufferMs}} ms · 已收 {{status.audio.mediaPackets}} 个媒体包</p>
      </section>
      <section class="ui-panel control-card" v-else>
        <div class="section-head"><h3>{{labels[status.hfp.call]??status.hfp.call}}</h3><span class="tag">HFP · {{labels[status.hfp.state]??status.hfp.state}}</span></div>
        <strong class="number">{{status.hfp.number||'等待来电'}}</strong>
        <div class="buttons"><MdButton :disabled="!canEdit||status.hfp.call!=='incoming'" @click="action('bt.call.answer')">接听</MdButton><MdButton variant="outlined" :disabled="!canEdit||status.hfp.call!=='incoming'" @click="action('bt.call.reject')">拒接</MdButton><MdButton variant="tonal" :disabled="!canEdit||status.hfp.call==='idle'" @click="action('bt.call.hangup')">挂断</MdButton></div>
        <p class="hint">{{status.hfp.codec}} · SCO 收 {{status.audio.scoRxPackets}} / 发 {{status.audio.scoTxPackets}}。通话采用远端说话时降低麦克风增益的门控。</p>
      </section>
      <section v-if="devices.length" class="ui-panel control-card"><h3>记住的设备</h3><div v-for="device in devices" :key="device.address" class="device-row"><span><strong>{{device.name||device.address}}</strong><small>{{device.address}}</small></span><span v-if="device.connected" class="tag">已连接</span><MdButton v-else variant="tonal" :disabled="!canConnect || device.connecting" @click="action('bt.connect',{address:device.address})">{{ device.connecting ? '正在连接…' : '连接' }}</MdButton><MdButton variant="text" :disabled="!canEdit||device.connected" @click="action('bt.forget',{address:device.address})">忘记</MdButton></div></section>
      <p v-if="connections.length >= maxConnections && maxConnections > 1" class="hint">连接已满。添加另一台设备前，请先断开一台。</p>
      <p v-if="status.error||status.audio.error" role="alert" class="service-error">控制器错误 {{status.error}} · 音频错误 {{status.audio.error}}</p>
    </template>
  </div>
</template>
<style scoped>
.connection-list { display: grid; gap: 14px; }
.connection-list .device-row { padding: 16px; border: 1px solid var(--md-outline-variant); border-radius: 14px; }
.connection-list .device-row.selected { border-color: var(--md-primary); background: color-mix(in srgb, var(--md-primary) 7%, transparent); }
.connection-list .device-row strong { font-size: 17px; }
.pairing-code { font: 600 32px var(--font-mono, monospace); letter-spacing: .22em; color: var(--md-primary); }
.bt-control{display:grid;gap:20px}.control-card{display:grid;gap:22px;padding:24px;min-width:0}.section-head,.device-row{display:flex;justify-content:space-between;align-items:center;gap:16px}h3,p{margin:0}.section-head h3{font-size:18px}.section-head p,.hint{font-size:13px;color:var(--md-on-surface-variant);line-height:1.7}.section-head p{margin-top:6px}.section-head>.ui-icon{color:var(--md-primary)}.buttons{display:flex;gap:10px;flex-wrap:wrap}.connected,.track{display:grid;gap:6px}.connected>strong,.track>strong,.number{font-size:22px}.connected>span,.track>span{color:var(--md-on-surface-variant);font-size:13px}.connected>.md-button{justify-self:start}.pairing{display:grid;gap:12px;padding:18px;border:1px solid var(--md-primary);border-radius:12px}.device-row>span{flex:1;min-width:0;overflow-wrap:anywhere}.device-row small{display:block;margin-top:4px;color:var(--md-on-surface-variant)}.service-error{display:grid;gap:8px;padding:16px;border:1px solid var(--md-error);border-radius:12px;color:var(--md-error);overflow-wrap:anywhere}@media(max-width:600px){.control-card{padding:18px}.device-row{flex-wrap:wrap}}
</style>
