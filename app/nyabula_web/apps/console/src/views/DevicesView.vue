<script setup lang="ts">
/* Device list: online dot, lastSeen, claim dialog, unbind. */
import { onMounted, ref } from 'vue';
import { MdButton, MdCard } from '@nyabula/ui';
import { useDevicesStore } from '../stores/devices';
import { useNavStore } from '../stores/nav';

const devices = useDevicesStore();
const nav = useNavStore();

const showClaim = ref(false);
const claimId = ref('');
const claimCode = ref('');
const claiming = ref(false);

onMounted(() => void devices.refresh());

async function doClaim() {
  if (!claimId.value || !claimCode.value) return;
  claiming.value = true;
  const ok = await devices.claim(claimId.value.trim(), claimCode.value.trim());
  claiming.value = false;
  if (ok) {
    showClaim.value = false;
    claimId.value = '';
    claimCode.value = '';
  }
}

async function doUnclaim(id: string) {
  if (!confirm(`确定解绑设备 ${id}？`)) return;
  await devices.unclaim(id);
}

function fmtLastSeen(s: string): string {
  if (!s) return '—';
  const d = new Date(s);
  return isNaN(d.getTime()) ? s : d.toLocaleString();
}
</script>

<template>
  <div class="page">
    <div class="head">
      <h2 class="title">设备</h2>
      <div class="head-actions">
        <MdButton variant="outlined" @click="devices.refresh()">刷新</MdButton>
        <MdButton @click="showClaim = true">认领设备</MdButton>
      </div>
    </div>

    <p v-if="devices.lastError" class="err">{{ devices.lastError }}</p>

    <div class="list">
      <MdCard v-for="d in devices.devices" :key="d.deviceId" class="dev-card">
        <div class="dev-row">
          <span class="online-dot" :class="{ on: d.online }" :title="d.online ? '在线' : '离线'" />
          <div class="dev-main" @click="nav.go('device', d.deviceId)">
            <span class="dev-name">{{ d.name || d.deviceId }}</span>
            <span class="dev-meta">{{ d.deviceId }} · core {{ d.coreVersion || '?' }} · 最后在线 {{ fmtLastSeen(d.lastSeen) }}</span>
          </div>
          <div class="dev-actions">
            <MdButton variant="tonal" @click="nav.go('device', d.deviceId)">详情</MdButton>
            <MdButton variant="text" @click="doUnclaim(d.deviceId)">解绑</MdButton>
          </div>
        </div>
      </MdCard>
      <p v-if="!devices.loading && devices.devices.length === 0" class="empty">
        还没有设备。点击「认领设备」，输入设备 ID 与认领码（模拟器打印在 stdout，真机显示在眼睛配对画面）。
      </p>
    </div>

    <!-- claim dialog -->
    <div v-if="showClaim" class="scrim" @click.self="showClaim = false">
      <MdCard title="认领设备" class="dialog">
        <label class="lbl">设备 ID</label>
        <input v-model="claimId" type="text" spellcheck="false" placeholder="nya-xxxx" />
        <label class="lbl top-gap">认领码</label>
        <input v-model="claimCode" type="text" spellcheck="false" placeholder="claimCode" @keyup.enter="doClaim" />
        <div class="row">
          <MdButton variant="text" @click="showClaim = false">取消</MdButton>
          <MdButton :disabled="claiming || !claimId || !claimCode" @click="doClaim">
            {{ claiming ? '认领中…' : '认领' }}
          </MdButton>
        </div>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.page {
  padding: 22px;
  max-width: 960px;
  margin: 0 auto;
}
.head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 18px;
}
.title {
  font-size: 22px;
}
.head-actions {
  display: flex;
  gap: 10px;
}
.list {
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.dev-row {
  display: flex;
  align-items: center;
  gap: 14px;
}
.online-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: var(--md-outline);
  flex: none;
}
.online-dot.on {
  background: var(--md-success);
  box-shadow: 0 0 8px var(--md-success);
}
.dev-main {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  gap: 3px;
  cursor: pointer;
}
.dev-name {
  font: 600 15px var(--font-body);
  color: var(--md-on-surface);
}
.dev-meta {
  font-size: 12px;
  color: var(--md-on-surface-variant);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.dev-actions {
  display: flex;
  gap: 8px;
  flex: none;
}
.empty {
  color: var(--md-on-surface-variant);
  font-size: 13.5px;
  text-align: center;
  margin-top: 30px;
}
.err {
  color: var(--md-error);
  font-size: 13px;
  margin-bottom: 12px;
}
.scrim {
  position: fixed;
  inset: 0;
  background: var(--md-scrim);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 10;
}
.dialog {
  width: min(400px, 90vw);
}
.lbl {
  display: block;
  font-size: 12.5px;
  color: var(--md-on-surface-variant);
  margin-bottom: 8px;
  letter-spacing: 1px;
}
.top-gap {
  margin-top: 14px;
}
.row {
  margin-top: 18px;
  display: flex;
  justify-content: flex-end;
  gap: 10px;
}
</style>
