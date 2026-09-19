<script setup lang="ts">
/* Device detail: stats cards + SVG bar charts + remote panel over the
 * Cloud relay (pure NyaLink after the ws upgrade — hello / pair / eye.*,
 * mirroring apps/panel behavior). */
import { computed, onMounted, onBeforeUnmount, ref, watch } from 'vue';
import { EyeCanvas, MdButton, MdCard, MdChip, PairOverlay, StatusPill } from '@nyabula/ui';
import { MODES } from '@nyabula/eye-engine';
import { deviceStats, type DeviceStats } from '../api/client';
import StatBars from '../components/StatBars.vue';
import { useRemoteStore } from '../stores/remote';
import { useNavStore } from '../stores/nav';

const props = defineProps<{ deviceId: string }>();

const remote = useRemoteStore();
const nav = useNavStore();

const stats = ref<DeviceStats | null>(null);
const statsError = ref<string | null>(null);

const MODE_LABELS: Record<string, string> = {
  idle: '待机', curious: '好奇', happy: '开心', processing: '处理中',
  star: '星星眼', heart: '爱心眼', sleepy: '困倦', sleep: '睡眠',
  angry: '生气', sad: '委屈', surprise: '惊讶', dizzy: '晕', derp: '呆',
};
/* Chip selection derives from the latest eye.state so every client stays
 * in sync; a click sets a pending (optimistic) value that the next remote
 * state supersedes. */
const pendingMode = ref<string | null>(null);
const activeMode = computed(
  () => pendingMode.value ?? remote.lastEyeState?.expression?.mode ?? 'idle',
);
watch(
  () => remote.lastEyeState,
  () => {
    pendingMode.value = null;
  },
);
const pairing = ref(false);
const pairError = ref<string | null>(null);

const connected = computed(() => remote.state === 'connected');
const needPair = computed(() => remote.state === 'pairing-required');
const showPairOverlay = computed(
  () => needPair.value || (pairing.value && remote.state === 'authenticating'),
);

const pillLabel = computed(() => {
  switch (remote.state) {
    case 'connected':
      return remote.device?.name ? `已接管 · ${remote.device.name}` : '已接管';
    case 'connecting':
      return '连接中…';
    case 'authenticating':
      return '鉴权中…';
    case 'pairing-required':
      return '待配对';
    case 'reconnecting':
      return '重连中…';
    case 'closed':
      return '已断开';
    default:
      return '未连接';
  }
});
const pillTone = computed(() =>
  remote.state === 'connected'
    ? 'ok'
    : ['connecting', 'authenticating', 'reconnecting'].includes(remote.state)
      ? 'busy'
      : 'off',
);

onMounted(async () => {
  remote.connect(props.deviceId);
  try {
    stats.value = await deviceStats(props.deviceId);
  } catch (e) {
    statsError.value = e instanceof Error ? e.message : String(e);
  }
});

onBeforeUnmount(() => remote.disconnect());

/* -------- stats formatting -------- */

function dayLabel(date: string): string {
  return date.slice(5); // "YYYY-MM-DD" -> "MM-DD"
}
const onlineBars = computed(() =>
  (stats.value?.daily ?? []).map((d) => ({ label: dayLabel(d.date), value: d.onlineSeconds })),
);
const frameBars = computed(() =>
  (stats.value?.daily ?? []).map((d) => ({ label: dayLabel(d.date), value: d.framesUp + d.framesDown })),
);
function fmtHours(sec: number): string {
  return `${(sec / 3600).toFixed(1)}h`;
}
function fmtFrames(n: number): string {
  return n.toLocaleString();
}

/* -------- remote panel actions -------- */

function onLook(data: { x?: number; y?: number; release?: boolean }) {
  void remote.send('eye.look', data);
}
function pickMode(mode: string) {
  pendingMode.value = mode;
  void remote.send('eye.mode', { mode });
}
async function doPair(code: string) {
  pairing.value = true;
  pairError.value = null;
  try {
    await remote.pair(code, 'Nyabula 控制台');
  } catch {
    pairError.value =
      remote.lastError && /wrong pairing code|EPERM/i.test(remote.lastError)
        ? '配对码错误'
        : (remote.lastError ?? '配对失败');
  } finally {
    pairing.value = false;
  }
}
</script>

<template>
  <div class="page">
    <div class="head">
      <MdButton variant="text" @click="nav.go('devices')">← 设备列表</MdButton>
      <h2 class="title">{{ remote.device?.name ?? deviceId }}</h2>
      <StatusPill :tone="pillTone" :label="pillLabel" />
    </div>

    <div class="grid">
      <MdCard title="日在线时长">
        <p v-if="statsError" class="err">{{ statsError }}</p>
        <StatBars v-else :items="onlineBars" :format="fmtHours" />
      </MdCard>
      <MdCard title="日转发帧数（上行 + 下行）">
        <p v-if="statsError" class="err">{{ statsError }}</p>
        <StatBars v-else :items="frameBars" :format="fmtFrames" color="var(--md-tertiary)" />
      </MdCard>
      <MdCard title="当前">
        <div class="now">
          <span class="now-item">
            状态：<b :class="{ ok: stats?.current.online }">{{ stats ? (stats.current.online ? '在线' : '离线') : '—' }}</b>
          </span>
          <span class="now-item">连接客户端：<b>{{ stats?.current.clients ?? '—' }}</b></span>
        </div>
      </MdCard>
    </div>

    <MdCard title="远程面板" class="remote-card">
      <div class="canvas-box">
        <EyeCanvas
          v-if="connected"
          :eye-state="remote.lastEyeState"
          :clock-offset-ms="remote.clockOffsetMs()"
          @look="onLook"
        />
        <div v-else class="eye-placeholder" />
        <PairOverlay v-if="showPairOverlay" :busy="pairing" :error="pairError" @submit="doPair" />
      </div>
      <p class="hint">点击 / 拖动画布 = 注视点（实时发 eye.look）</p>

      <div class="chips">
        <MdChip
          v-for="m in MODES"
          :key="m"
          :selected="activeMode === m"
          @click="pickMode(m)"
        >
          {{ MODE_LABELS[m] ?? m }}
        </MdChip>
      </div>
      <p v-if="!connected && !needPair" class="offline-hint">
        设备未接管（离线或连接中）。设备在线时会自动经 Cloud relay 建立 NyaLink。
      </p>
      <p v-if="remote.lastError" class="err">{{ remote.lastError }}</p>
    </MdCard>
  </div>
</template>

<style scoped>
.page {
  padding: 22px;
  max-width: 1080px;
  margin: 0 auto;
}
.head {
  display: flex;
  align-items: center;
  gap: 14px;
  margin-bottom: 18px;
}
.title {
  font-size: 21px;
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
  gap: 16px;
  margin-bottom: 16px;
}
.now {
  display: flex;
  flex-direction: column;
  gap: 10px;
  font-size: 14px;
  color: var(--md-on-surface-variant);
}
.now-item b {
  color: var(--md-on-surface);
}
.now-item b.ok {
  color: var(--md-primary);
}
.remote-card {
  margin-top: 4px;
}
.canvas-box {
  position: relative;
  height: 340px;
  margin-top: 4px;
}
.eye-placeholder {
  width: 100%;
  height: 100%;
  border-radius: var(--radius-l);
}
.hint {
  color: var(--md-on-surface-variant);
  font-size: 12px;
  text-align: center;
  margin: 10px 0 14px;
}
.chips {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}
.offline-hint {
  color: var(--md-on-surface-variant);
  font-size: 13px;
  margin: 14px 0 0;
}
.err {
  color: var(--md-error);
  font-size: 13px;
  margin: 12px 0 0;
}
</style>
