<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { MdButton, MdTextField, NkSliderRow, NkToggleRow, UiIcon, describeError } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useVisiblePoll } from '../../../composables/usePageVisible';
interface Probe {
  id: number; mode: 'none' | 'wake' | 'microphone'; state: string; active: boolean;
  remainingMs: number; durationMs: number; samples: number; rms: number; maxRms: number;
  peak: number; clippedSamples: number; detections: number; word: string; error: number;
}
interface VoiceStatus {
  enabled: boolean; running: boolean; state: string; streaming: boolean;
  models: { kws: { state: string; error: number } };
  microphone: { open: boolean; level: number; blockedByPlayback?: boolean; claims: number };
  config: { wakeSensitivity: number; wakePhrase?: string; applying?: boolean };
  errors: { last: number; text: string; capture: number };
  test?: Probe;
}
interface AudioStatus { input?: { muted: boolean; gainDb: number; route: string } }
const session = useSessionStore();
const status = ref<VoiceStatus>();
const audio = ref<AudioStatus>();
const phrase = ref('');
const sensitivity = ref(70);
const draft = ref(false);
const busy = ref(false);
const error = ref('');
const readError = ref('');
const saved = ref(false);
let refreshing = false, revision = 0;
const test = computed(() => status.value?.test);
const testing = computed(() => !!test.value?.active);
const editable = computed(() => session.connected && session.isOwner && !busy.value && !testing.value && !!status.value);
const blocked = computed(() => !!status.value?.microphone.blockedByPlayback || !!status.value?.microphone.claims);
const muted = computed(() => audio.value?.input?.muted === true);
const canTest = computed(() => editable.value && !!test.value && !blocked.value && !muted.value && !status.value?.config.applying && ['idle', 'off'].includes(status.value?.state ?? ''));
const wordError = computed(() => {
  const text = phrase.value.trim();
  if ([...text].length > 24) return '最多 24 个字符，建议使用简短短语。';
  if (text && !/^[A-Za-z \u3400-\u4dbf\u4e00-\u9fff\u{20000}-\u{2fa1f}]+$/u.test(text)) return '请使用中文、英文字母或空格，不包含数字、标点或表情。';
  return '';
});
const stateText = computed(() => {
  if (testing.value) return test.value?.state === 'preparing' ? '正在准备测试' : '正在测试';
  if (!status.value?.enabled) return '语音唤醒已关闭';
  if (muted.value) return '设备麦克风已静音';
  if (blocked.value) return '音频播放占用麦克风，唤醒暂时暂停';
  if (status.value?.errors.capture) return '麦克风暂不可用';
  if (status.value?.models.kws.state === 'error') return '唤醒模型未就绪';
  if (status.value?.config.applying || !status.value?.streaming) return '正在准备唤醒模型';
  return status.value?.microphone.open ? '正在监听唤醒词' : '正在打开麦克风';
});
const db = (sample: number) => Math.max(-96, 20 * Math.log10(Math.max(1, sample) / 32768));
const levelDb = computed(() => status.value?.microphone.open && !muted.value ? db(status.value.microphone.level) : -96);
const meter = computed(() => Math.max(0, Math.min(100, (levelDb.value + 60) / .6)));
const seconds = computed(() => Math.ceil((test.value?.remainingMs ?? 0) / 1000));
const wordLabel = (word: string) => word === 'nihao_openvela' ? '你好，openvela' : word === 'hello_openvela' ? 'Hello, openvela' : word === 'custom' ? status.value?.config.wakePhrase || '自定义唤醒词' : word;
function setPhrase(value: string) { phrase.value = value; draft.value = true; saved.value = false; }
function setSensitivity(value: number) { sensitivity.value = value; draft.value = true; saved.value = false; }
function resetDraft() { phrase.value = status.value?.config.wakePhrase ?? ''; sensitivity.value = status.value?.config.wakeSensitivity ?? 70; draft.value = false; }
async function refresh() {
  if (!session.connected || refreshing || busy.value) return;
  refreshing = true; const version = revision, client = session.client;
  try {
    const next = await session.request('voice.status') as unknown as VoiceStatus;
    if (!next.microphone || !next.config || !next.models?.kws) throw Error('设备未提供有效的唤醒状态');
    const nextAudio = await session.request('audio.status') as unknown as AudioStatus;
    if (version !== revision || client !== session.client) return;
    status.value = next; audio.value = nextAudio; readError.value = '';
    if (!draft.value) resetDraft();
  } catch (e) { if (version === revision && client === session.client) readError.value = describeError(e); }
  finally { refreshing = false; }
}
async function action(topic: string, data: Record<string, unknown>) {
  if (busy.value) return false;
  busy.value = true; error.value = ''; const version = ++revision, client = session.client;
  try {
    const next = await session.request(topic, data, { timeoutMs: topic === 'voice.config' ? 30000 : 8000 }) as unknown as VoiceStatus;
    if (version !== revision || client !== session.client) return false;
    status.value = next;
    if (topic === 'voice.config') { draft.value = false; resetDraft(); saved.value = true; }
    return true;
  } catch (e) { if (version === revision && client === session.client) error.value = describeError(e); return false; }
  finally { if (version === revision) { busy.value = false; void refresh(); } }
}
function save() {
  if (!editable.value || wordError.value) return;
  void action('voice.config', { wakeSensitivity: sensitivity.value, wakePhrase: phrase.value.trim() });
}
function start(mode: 'wake' | 'microphone') {
  if (canTest.value) void action('voice.test.start', { mode, durationMs: mode === 'wake' ? 20000 : 5000 });
}
watch(() => session.client, () => {
  revision++; busy.value = false; status.value = undefined; audio.value = undefined;
  draft.value = false; saved.value = false; error.value = ''; readError.value = ''; void refresh();
}, { immediate: true });
useVisiblePoll(() => void refresh(), 500);
</script>

<template>
  <div class="wake-control">
    <div v-if="readError" class="wake-error" role="alert"><strong>暂时无法读取唤醒状态</strong><span>{{ readError }}</span><MdButton variant="text" @click="refresh">重试</MdButton></div>
    <div v-if="error" class="wake-error" role="alert">{{ error }}</div>
    <template v-if="status">
      <section class="ui-panel wake-card">
        <div class="wake-head"><div><h3>唤醒设置</h3><p role="status">{{ stateText }}</p></div><UiIcon name="mic" :size="28" /></div>
        <NkToggleRow :model-value="status.enabled" icon="mic" title="语音唤醒" sub="开启后等待唤醒词，再开始对话" :disabled="!editable" @update:model-value="value => action('voice.enable', { enabled: value })" />
        <p class="wake-hint">默认唤醒词：<strong>你好，openvela</strong> / <strong>Hello, openvela</strong></p>
        <MdTextField :model-value="phrase" label="额外唤醒词（可选）" placeholder="例如：小喵小喵" :error="wordError" hint="中文或英文短语；清空即可只保留默认唤醒词。" :disabled="!editable || status.config.wakePhrase === undefined" @update:model-value="setPhrase" />
        <NkSliderRow :model-value="sensitivity" title="唤醒灵敏度" :min="0" :max="100" :step="1" :disabled="!editable" @update:model-value="setSensitivity" />
        <p class="wake-hint">数值越高越容易唤醒，也越容易误触发。先测试，再调整。</p>
        <div class="wake-actions"><MdButton :disabled="!editable || !draft || !!wordError" @click="save">{{ busy ? '正在处理…' : '保存设置' }}</MdButton><MdButton v-if="draft" variant="text" :disabled="busy" @click="resetDraft">撤销修改</MdButton><span v-if="saved" role="status">{{ status.config.applying ? '已保存，正在应用…' : '设置已保存' }}</span></div>
      </section>
      <p v-if="blocked" class="wake-notice">当前硬件模式在播放或通话时会暂停麦克风。请先停止音频，再测试唤醒。</p>
      <p v-else-if="muted" class="wake-notice">麦克风已静音，请先在“音频路由”中取消麦克风静音。</p>
      <section class="ui-panel wake-card">
        <div class="wake-head"><h3>测试唤醒</h3><span v-if="testing && test?.mode === 'wake'">{{ test.state === 'preparing' ? '准备中' : `剩余 ${seconds} 秒` }}</span></div>
        <p class="wake-hint">说出唤醒词。测试只记录命中，不会开始对话。</p>
        <div class="wake-actions"><MdButton :disabled="!canTest" @click="start('wake')">开始 20 秒测试</MdButton><MdButton v-if="testing && test?.mode === 'wake'" variant="outlined" :disabled="busy" @click="action('voice.test.stop', {})">停止测试</MdButton></div>
        <div v-if="test?.mode === 'wake'" class="wake-result" aria-live="polite"><strong>命中 {{ test.detections }} 次</strong><span v-if="test.word">最近命中：{{ wordLabel(test.word) }}</span><span v-if="test.state === 'done'">测试结束</span></div>
      </section>
      <section class="ui-panel wake-card">
        <div class="wake-head"><h3>测试设备麦克风</h3><span v-if="testing && test?.mode === 'microphone'">{{ test.state === 'preparing' ? '准备中' : `剩余 ${seconds} 秒` }}</span></div>
        <div class="wake-meter" role="meter" aria-label="设备麦克风电平" :aria-valuenow="Math.max(-60, levelDb)" :aria-valuemin="-60" :aria-valuemax="0"><i :style="{ width: `${meter}%` }" :class="{ hot: meter > 95 }" /></div>
        <div class="wake-meter-label"><span>{{ status.microphone.open && !muted ? `${Math.round(levelDb)} dBFS` : '输入暂停' }}</span><span v-if="audio?.input">输入增益 {{ audio.input.gainDb }} dB</span></div>
        <p class="wake-hint">请对设备说话，观察电平是否明显上升。峰值接近 0 dBFS 时容易失真。</p>
        <div class="wake-actions"><MdButton :disabled="!canTest" @click="start('microphone')">开始 5 秒测试</MdButton><MdButton v-if="testing && test?.mode === 'microphone'" variant="outlined" :disabled="busy" @click="action('voice.test.stop', {})">停止测试</MdButton></div>
        <div v-if="test?.mode === 'microphone'" class="wake-result" aria-live="polite"><strong>{{ test.samples ? `峰值 ${Math.round(db(test.peak))} dBFS` : '等待设备音频' }}</strong><span>收到 {{ test.samples }} 个采样 · 过载 {{ test.clippedSamples }} 个采样</span><span v-if="test.state === 'done'">测试结束</span></div>
      </section>
      <p v-if="test?.state === 'error'" class="wake-error" role="alert">测试未完成（{{ test.error }}）。请检查麦克风是否可用、唤醒模型是否就绪。</p>
      <p v-if="!test" class="wake-notice">当前固件尚未提供唤醒与麦克风测试接口。</p>
      <p v-if="!session.isOwner" class="wake-hint">以主人身份连接后可修改设置和开始测试。</p>
    </template>
  </div>
</template>

<style scoped>
.wake-control { display:grid; gap:20px; }
.wake-card { display:grid; gap:20px; padding:24px; min-width:0; }
.wake-head { display:flex; justify-content:space-between; align-items:center; gap:16px; }
h3,p { margin:0; }
h3 { font-size:18px; }
.wake-head p { margin-top:6px; color:var(--md-on-surface-variant); }
.wake-head>.ui-icon { color:var(--md-primary); }
.wake-hint,.wake-meter-label,.wake-head>span { font-size:14px; line-height:1.7; color:var(--md-on-surface-variant); }
.wake-hint strong { color:var(--md-on-surface); font-weight:500; }
.wake-actions { display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
.wake-actions>span { color:var(--md-primary); font-size:14px; }
.wake-meter { height:18px; overflow:hidden; border-radius:9px; background:var(--md-surface-container-highest); }
.wake-meter i { display:block; height:100%; border-radius:inherit; background:var(--md-primary); transition:width .15s ease; }
.wake-meter i.hot { background:var(--md-error); }
.wake-meter-label { display:flex; justify-content:space-between; gap:16px; }
.wake-result { display:grid; gap:6px; padding:16px; border-radius:12px; background:var(--md-surface-container-high); }
.wake-result strong { font-size:20px; }
.wake-result span { font-size:14px; color:var(--md-on-surface-variant); }
.wake-error,.wake-notice { padding:16px; border-radius:12px; font-size:14px; line-height:1.7; }
.wake-error { display:grid; gap:8px; color:var(--md-error); border:1px solid var(--md-error); overflow-wrap:anywhere; }
.wake-notice { background:var(--md-surface-container-high); color:var(--md-on-surface-variant); }
@media(max-width:600px) { .wake-card { padding:18px; } }
</style>
