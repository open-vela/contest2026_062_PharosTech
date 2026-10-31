<script setup lang="ts">
/* Settings: device info (sys.info), cloud relay card (cloud.status display +
 * cloud.config form, owner-only), disconnect / forget device. */
import { computed, onMounted, ref } from 'vue';
import { MdButton, MdCard } from '@nyabula/ui';
import { useLinkStore } from '../stores/link';

const link = useLinkStore();
const isOwner = computed(() => link.role === 'owner');

interface SysInfo {
  device?: { id?: string; name?: string; coreVersion?: string };
  uptime?: number;
  battery?: number;
  wifi?: { ssid?: string; rssi?: number } | string;
}
const info = ref<SysInfo | null>(null);
const infoError = ref<string | null>(null);

interface CloudStatus {
  enabled?: boolean;
  url?: string;
  connected?: boolean;
  deviceId?: string;
  claimCode?: string;
}
const cloud = ref<CloudStatus | null>(null);
const cloudError = ref<string | null>(null);
const cloudEnabled = ref(false);
const cloudUrl = ref('');
const cloudSaving = ref(false);
const cloudSaved = ref(false);

function fmtUptime(s: number | undefined): string {
  if (typeof s !== 'number') return '—';
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h > 0 ? `${h} 小时 ${m} 分` : `${m} 分`;
}

function wifiText(w: SysInfo['wifi']): string {
  if (!w) return '—';
  if (typeof w === 'string') return w;
  return `${w.ssid ?? '—'}${typeof w.rssi === 'number' ? ` (${w.rssi} dBm)` : ''}`;
}

async function loadInfo(): Promise<void> {
  infoError.value = null;
  try {
    info.value = (await link.request('sys.info', {})) as SysInfo;
  } catch (e) {
    infoError.value = e instanceof Error ? e.message : String(e);
  }
}

async function loadCloud(): Promise<void> {
  cloudError.value = null;
  try {
    const s = (await link.request('cloud.status', {})) as CloudStatus;
    cloud.value = s;
    cloudEnabled.value = s.enabled === true;
    cloudUrl.value = s.url ?? '';
  } catch (e) {
    cloudError.value = e instanceof Error ? e.message : String(e);
  }
}

async function saveCloud(): Promise<void> {
  if (!isOwner.value) return;
  cloudSaving.value = true;
  cloudError.value = null;
  cloudSaved.value = false;
  try {
    await link.request('cloud.config', { enabled: cloudEnabled.value, url: cloudUrl.value.trim() });
    cloudSaved.value = true;
    await loadCloud();
  } catch (e) {
    cloudError.value = e instanceof Error ? e.message : String(e);
  } finally {
    cloudSaving.value = false;
  }
}

function forgetDevice(): void {
  if (window.confirm('忘记该设备？将清除已保存的配对令牌，需重新配对。')) {
    link.forget();
  }
}

onMounted(() => {
  void loadInfo();
  void loadCloud();
});
</script>

<template>
  <div class="settings-view">
    <h2 class="title">设置</h2>

    <div class="cards">
      <MdCard title="设备信息">
        <p v-if="infoError" class="err">{{ infoError }}</p>
        <dl v-else class="kv">
          <div><dt>名称</dt><dd>{{ info?.device?.name ?? link.device?.name ?? '—' }}</dd></div>
          <div><dt>设备 ID</dt><dd>{{ info?.device?.id ?? link.device?.id ?? '—' }}</dd></div>
          <div><dt>核心版本</dt><dd>{{ info?.device?.coreVersion ?? link.device?.coreVersion ?? '—' }}</dd></div>
          <div><dt>运行时长</dt><dd>{{ fmtUptime(info?.uptime) }}</dd></div>
          <div><dt>电量</dt><dd>{{ typeof info?.battery === 'number' ? info.battery + '%' : '—' }}</dd></div>
          <div><dt>WiFi</dt><dd>{{ wifiText(info?.wifi) }}</dd></div>
          <div><dt>我的角色</dt><dd>{{ link.role ?? '—' }}</dd></div>
        </dl>
      </MdCard>

      <MdCard title="云中继">
        <p v-if="cloudError" class="err">{{ cloudError }}</p>
        <template v-if="cloud">
          <dl class="kv">
            <div><dt>状态</dt>
              <dd :class="cloud.connected ? 'ok' : 'muted'">{{ cloud.connected ? '已连接' : '未连接' }}</dd>
            </div>
            <div v-if="cloud.claimCode"><dt>认领码</dt><dd><code>{{ cloud.claimCode }}</code></dd></div>
          </dl>
          <div class="cloud-form">
            <label class="row">
              <span>启用云中继</span>
              <input v-model="cloudEnabled" type="checkbox" :disabled="!isOwner" />
            </label>
            <label class="col">
              <span>Cloud 地址</span>
              <input
                v-model="cloudUrl"
                type="text"
                spellcheck="false"
                placeholder="wss://cloud.nyabula.tech"
                :disabled="!isOwner"
              />
            </label>
            <div class="row-end">
              <span v-if="cloudSaved" class="saved">已保存</span>
              <MdButton variant="tonal" :disabled="!isOwner || cloudSaving" @click="saveCloud">
                {{ cloudSaving ? '保存中…' : '保存' }}
              </MdButton>
            </div>
            <p v-if="!isOwner" class="muted small">仅 owner 可修改云配置。</p>
          </div>
        </template>
        <p v-else-if="!cloudError" class="muted">加载中…</p>
      </MdCard>

      <MdCard title="连接">
        <div class="conn-actions">
          <MdButton variant="outlined" @click="link.disconnect()">断开连接</MdButton>
          <MdButton variant="text" class="danger" @click="forgetDevice">忘记设备</MdButton>
        </div>
        <p class="muted small">忘记设备会删除本机保存的配对令牌。</p>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.settings-view {
  max-width: 720px;
  margin: 0 auto;
  padding: 18px 20px 40px;
}
.title {
  font: 600 19px var(--font-title);
  color: var(--md-on-surface);
  margin-bottom: 14px;
}
.cards { display: flex; flex-direction: column; gap: 14px; }
.err { color: var(--md-error); font-size: 13px; }
.muted { color: var(--md-on-surface-variant); font-size: 14px; }
.small { font-size: 12px; margin-top: 8px; }
.ok { color: var(--md-success); }
.kv { display: flex; flex-direction: column; gap: 8px; }
.kv > div { display: flex; justify-content: space-between; gap: 16px; }
.kv dt { color: var(--md-on-surface-variant); font-size: 13px; }
.kv dd { color: var(--md-on-surface); font-size: 13px; font-weight: 600; word-break: break-all; text-align: right; }
.cloud-form { margin-top: 14px; display: flex; flex-direction: column; gap: 12px; }
.cloud-form .row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  font-size: 14px;
  color: var(--md-on-surface);
}
.cloud-form input[type='checkbox'] { accent-color: var(--md-primary); width: 18px; height: 18px; }
.cloud-form .col { display: flex; flex-direction: column; gap: 6px; font-size: 13px; color: var(--md-on-surface-variant); }
.cloud-form input[type='text'] {
  background: var(--md-surface-container-high);
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-s);
  color: var(--md-on-surface);
  font: 400 14px var(--font-body);
  padding: 9px 12px;
  outline: none;
}
.cloud-form input[type='text']:focus { border-color: var(--md-primary); }
.cloud-form input:disabled { opacity: 0.5; }
.row-end { display: flex; align-items: center; justify-content: flex-end; gap: 10px; }
.saved { color: var(--md-success); font-size: 13px; }
.conn-actions { display: flex; gap: 10px; flex-wrap: wrap; }
.danger { color: var(--md-error) !important; }
</style>
