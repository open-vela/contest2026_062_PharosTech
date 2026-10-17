<script setup lang="ts">
/* 本机模型: the LLM in the compute domain, as the agent sees it.
 *
 *   agent.config.ondevice.get / .set   is it one of the router's backends
 *   compute.status                     is it loaded, what is it doing
 *   compute.llm.load / .unload         bring it into RAM ahead of the first question
 *   models.list                        are the two files it needs on the device
 *
 * A firmware without agent.config.ondevice.* (ENOTFOUND) shows nothing here.
 * The agent answers EBUSY while it starts and while a run is active: the
 * request is made again a few times before the owner is told. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useRoute } from 'vue-router';
import { MdButton, MdCard, MdSwitch, UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';
import { useNyabotStore } from '../../stores/nyabot';
import { useComputeStatus } from '../../composables/useComputeStatus';
import {
  blobPercent, errorCode, lastRunText, llmStatusText, llmStatusTone, missingLlmFiles, onDeviceErrorText, parseOnDevice, retryOnBusy,
  type OnDeviceConfig,
} from '../../lib/deviceCompute';
import { parseModelsList } from '../../lib/deviceModels';

const props = defineProps<{ reloadKey?: number }>();
const emit = defineEmits<{ (e: 'changed'): void }>();

const session = useSessionStore();
const bot = useNyabotStore();
const route = useRoute();
const compute = useComputeStatus();

const config = ref<OnDeviceConfig | null>(null);
/** agent.config.ondevice.* is not in this firmware. */
const hidden = ref(false);
const busy = ref(false);
const acting = ref<'load' | 'unload' | null>(null);
const waiting = ref('');
const error = ref('');
const missing = ref<{ path: string; label: string }[]>([]);
let alive = true;

const owner = computed(() => session.connected && session.isOwner);
const status = computed(() => compute.status.value);
const state = computed(() => status.value?.llm.state ?? 'unknown');
const linked = computed(() => !!status.value?.linked);
const moving = computed(() => state.value === 'provisioning' && !!status.value && status.value.blob.size > 0);
const canToggle = computed(() => owner.value && !!config.value?.available && !busy.value && !bot.status?.activeRun);
const canLoad = computed(() => owner.value && linked.value && !acting.value && !missing.value.length
  && (state.value === 'unloaded' || state.value === 'error'));
const canUnload = computed(() => owner.value && linked.value && !acting.value && (state.value === 'ready' || state.value === 'error'));
const runStats = computed(() => (status.value ? lastRunText(status.value.llm) : ''));
const modelsLink = computed(() => ({ name: 'device', params: { key: route.params.key, section: 'models' } }));

function waitText(): string {
  return bot.status?.activeRun ? '对话正在运行，等它结束…' : '智能体正在启动…';
}

async function ondevice(action: 'get' | 'set', data: Record<string, unknown> = {}): Promise<void> {
  if (!owner.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const raw = await retryOnBusy(() => session.request('agent.config.ondevice.' + action, data), {
      onRetry: () => { waiting.value = waitText(); },
      cancelled: () => !alive || client !== session.client,
    });
    if (client !== session.client) return;
    config.value = parseOnDevice(raw);
    hidden.value = false;
    error.value = '';
    if (action === 'set') {
      emit('changed');
      await bot.refresh();
    }
  } catch (e) {
    if (client !== session.client) return;
    if (errorCode(e) === 'ENOTFOUND') hidden.value = true;
    else if (errorCode(e) === 'EEXIST' && config.value) {
      config.value = { ...config.value, available: false, enabled: false };
      error.value = '';
    } else error.value = onDeviceErrorText(e);
  } finally {
    busy.value = false;
    waiting.value = '';
    // The connection changed while this one waited: what it asked is void, ask the new one.
    if (alive && client !== session.client) void ondevice('get');
  }
}

function toggle(enabled: boolean): void {
  if (!canToggle.value || !config.value) return;
  void ondevice('set', { enabled, priority: config.value.priority });
}

function setPriority(event: Event): void {
  const value = Number((event.target as HTMLInputElement).value);
  if (!canToggle.value || !config.value || !Number.isInteger(value) || value < 0 || value > 100 || value === config.value.priority) return;
  void ondevice('set', { enabled: config.value.enabled, priority: value });
}

async function checkFiles(): Promise<void> {
  if (!session.connected) return;
  const client = session.client;
  try {
    const list = parseModelsList(await session.request('models.list'));
    if (client === session.client) missing.value = missingLlmFiles(list.items);
  } catch {
    // Unknown is not "missing": the load itself says so when a file is not there.
    if (client === session.client) missing.value = [];
  }
}

async function act(action: 'load' | 'unload'): Promise<void> {
  if (acting.value || !owner.value) return;
  const client = session.client;
  acting.value = action;
  error.value = '';
  try {
    const raw = await retryOnBusy(() => session.request('compute.llm.' + action, {}, { timeoutMs: 15000 }), {
      onRetry: () => { waiting.value = action === 'unload' ? '模型正在使用，等它结束…' : '计算域正忙，稍后重试…'; },
      cancelled: () => !alive || client !== session.client,
    });
    if (client === session.client) compute.accept(raw);
  } catch (e) {
    if (client !== session.client) return;
    error.value = errorCode(e) === 'ENOTFOUND' ? '此固件不支持预加载／卸载本机模型' : onDeviceErrorText(e);
    if (errorCode(e) === 'ENOENT') void checkFiles();
    void compute.refresh();
  } finally {
    acting.value = null;
    waiting.value = '';
  }
}

function refreshAll(): void {
  void ondevice('get');
  void checkFiles();
  void compute.refresh();
}

watch([() => session.client, owner, () => bot.status?.ready], (now, old) => {
  if (!old || now[0] !== old[0]) config.value = null;
  void ondevice('get');
  void checkFiles();
}, { immediate: true });
/* The router panel moved or removed a backend: the slot may be free or taken now. */
watch(() => props.reloadKey, () => void ondevice('get'));
/* A load that ended in an error is most often a file that is not there. */
watch(state, (now) => { if (now === 'error' || now === 'ready') void checkFiles(); });

onBeforeUnmount(() => { alive = false; });
</script>

<template>
  <MdCard v-if="!hidden && (config || status)" id="local-model" class="local-model">
    <div class="head">
      <span class="icon"><UiIcon name="cloud_off" :size="20" /></span>
      <div class="title">
        <h3>本机模型</h3>
        <p class="muted mono model">{{ config?.model || status?.llm.model || 'llm/model.rkllm' }}</p>
      </div>
      <MdSwitch v-if="config?.available" :model-value="config.enabled" :disabled="!canToggle" label="启用本机模型" @update:model-value="toggle" />
    </div>

    <p v-if="!session.connected" class="line muted">设备未连接。</p>
    <p v-else-if="!session.isOwner" class="line muted">启用、预加载和卸载需要主人权限；下面的状态仅供查看。</p>
    <p v-if="waiting" class="line" role="status">{{ waiting }}</p>
    <p v-if="error" class="line error" role="alert">{{ error }}</p>

    <div v-if="config && !config.available" class="notice">
      <UiIcon name="warning" :size="18" />
      <p>路由槽位 {{ config.slot + 1 }} 已被你添加的自定义后端占用，本机模型无法启用。请在下方「多模型路由」里删除该后端（需要的话再重新添加，它会落到靠前的空槽位），然后点这里的「刷新」。</p>
    </div>

    <div v-if="missing.length" class="notice">
      <UiIcon name="warning" :size="18" />
      <p>设备上缺少{{ missing.map((f) => `「${f.label}」`).join('和') }}，本机模型无法加载。
        <RouterLink :to="modelsLink">去「模型」页上传</RouterLink></p>
    </div>

    <template v-if="compute.unsupported.value">
      <p class="line muted">此固件没有计算域（普通单系统固件），本机模型不可用；刷入带计算域的 AMP 固件后这里会显示加载状态。</p>
    </template>
    <template v-else>
      <div class="status" role="status">
        <span class="tag" :class="status ? llmStatusTone(status) : 'info'">{{ status ? llmStatusText(status) : compute.failed.value ? '读取状态失败' : '正在读取状态…' }}</span>
        <span v-if="state === 'loading'" class="muted hint">约 10 秒</span>
        <span v-else-if="state === 'provisioning'" class="muted hint">模型约 875 MB，从闪存搬入计算域内存，通常 20–80 秒</span>
      </div>
      <div v-if="state === 'provisioning' || state === 'loading'" class="bar" :class="{ indeterminate: !moving }" role="progressbar"
        aria-valuemin="0" aria-valuemax="100" :aria-valuenow="moving && status ? blobPercent(status.blob) : undefined" aria-label="本机模型加载进度">
        <span :style="{ width: (moving && status ? blobPercent(status.blob) : 100) + '%' }" />
      </div>

      <dl class="stats">
        <div><dt>上次推理</dt><dd>{{ runStats || '还没有推理记录' }}</dd></div>
        <div v-if="config?.available"><dt>路由统计</dt>
          <dd>成功 {{ config.calls }} · 失败 {{ config.failures }} · 平滑延迟 {{ config.latencyMs ? `${config.latencyMs} ms` : '—' }}（本次运行统计）</dd></div>
        <div v-if="config?.available"><dt>路由优先级</dt>
          <dd><input type="number" min="0" max="100" step="1" :value="config.priority" :disabled="!canToggle" aria-label="本机模型路由优先级" @change="setPriority" />
            <span class="muted hint">数值越小越优先；路由槽位 {{ config.slot + 1 }}</span></dd></div>
      </dl>

      <div class="actions">
        <MdButton variant="tonal" :disabled="!canLoad" @click="act('load')">{{ acting === 'load' ? '正在请求…' : '预加载' }}</MdButton>
        <MdButton variant="outlined" :disabled="!canUnload" @click="act('unload')">{{ acting === 'unload' ? '正在卸载…' : '卸载' }}</MdButton>
        <MdButton variant="text" :disabled="!session.connected || busy" @click="refreshAll"><UiIcon name="refresh" :size="16" /> 刷新</MdButton>
      </div>
    </template>

    <p class="line muted explain">完全离线运行，对话内容不出设备，也不产生模型费用。这是一个 1B 的小模型：擅长日常聊天和简单的设备指令（计时、闹钟、音量、表情、音乐、备忘、待办），不擅长复杂推理、长文写作和多步任务。未预加载时，第一次对话会先搬运并加载模型，要多等一分钟左右；卸载可让出约 1 GB 计算域内存。如果同时配置了云端后端，路由会按策略选择，并在一方失败时自动切换到另一方。</p>
    <p v-if="bot.status?.activeRun" class="line muted">对话正在运行，完成后可修改启用状态。</p>
  </MdCard>
</template>

<style scoped>
.local-model { margin-top: 28px; line-height: 1.6; }
.head { display: flex; align-items: center; flex-wrap: wrap; gap: 10px 12px; margin-bottom: 10px; }
.icon {
  width: 36px; height: 36px; flex: none; border-radius: var(--radius-m);
  display: grid; place-items: center;
  background: var(--md-secondary-container); color: var(--md-on-secondary-container);
}
.title { flex: 1 1 160px; min-width: 0; }
.title h3 { margin: 0; font: 600 16px var(--font-title); color: var(--md-on-surface); }
.model { margin: 0; font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.line { font-size: 13px; margin: 6px 0 0; }
.error { color: var(--md-error); overflow-wrap: anywhere; }
.explain { margin-top: 12px; }
.notice { display: flex; align-items: flex-start; gap: 10px; margin: 10px 0; padding: 10px 12px; border-radius: 12px; background: var(--md-surface-container-high); }
.notice > .ui-icon { flex: none; margin-top: 3px; color: var(--md-warning); }
.notice p { margin: 0; font-size: 13px; overflow-wrap: anywhere; }
.status { display: flex; align-items: center; flex-wrap: wrap; gap: 6px 10px; margin-top: 10px; min-width: 0; }
.status .tag { white-space: normal; overflow-wrap: anywhere; }
.hint { font-size: 12.5px; }
.bar { height: 6px; margin-top: 10px; border-radius: 3px; background: var(--md-outline-variant); overflow: hidden; }
.bar > span { display: block; height: 100%; background: var(--md-primary); transition: width 0.3s linear; }
.bar.indeterminate > span { animation: local-model-pulse 1.2s ease-in-out infinite; }
@keyframes local-model-pulse { 0%, 100% { opacity: 0.35; } 50% { opacity: 1; } }
.stats { margin: 12px 0 0; display: flex; flex-direction: column; gap: 6px; font-size: 13px; }
.stats > div { display: flex; flex-wrap: wrap; gap: 2px 12px; }
.stats dt { flex: none; width: 5.5em; color: var(--md-on-surface-variant); }
.stats dd { margin: 0; flex: 1 1 200px; min-width: 0; overflow-wrap: anywhere; display: flex; flex-wrap: wrap; align-items: center; gap: 6px 10px; }
.stats input { width: 72px; min-height: 36px; border: 1px solid var(--md-outline); border-radius: 8px; padding: 4px 8px; background: var(--md-surface); color: var(--md-on-surface); font: inherit; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
