<script setup lang="ts">
/* Device list: online dot, lastSeen, claim dialog, unbind. */
import { onMounted, ref } from 'vue';
import { EmptyState, MdButton, MdCard } from '@nyabula/ui';
import { useFleet } from '../../logic/useFleet';
import { fmtDateTime } from '../../logic/format';

const fleet = useFleet();
const { devices, nav } = fleet;

const showClaim = ref(false);
const claimId = ref('');
const claimCode = ref('');
const claiming = ref(false);

onMounted(() => void fleet.refresh());

async function doClaim() {
  if (claiming.value || !claimId.value.trim() || !claimCode.value.trim()) return;
  claiming.value = true;
  const ok = await fleet.claim(claimId.value, claimCode.value);
  claiming.value = false;
  if (ok) {
    showClaim.value = false;
    claimId.value = '';
    claimCode.value = '';
  }
}
</script>

<template>
  <div class="page">
    <div class="head">
      <h2 class="title">设备</h2>
      <div class="head-actions">
        <MdButton variant="outlined" @click="fleet.refresh()">刷新</MdButton>
        <MdButton @click="showClaim = true">认领设备</MdButton>
      </div>
    </div>

    <p v-if="devices.lastError && !showClaim" class="err">{{ devices.lastError }}</p>

    <div class="filters">
      <input v-model="fleet.q.value" type="search" aria-label="搜索设备" placeholder="搜索名称、ID、版本" />
      <label>状态 <select v-model="fleet.filter.value"><option value="all">全部</option><option value="on">在线</option><option value="off">离线</option></select></label>
      <label>排序 <select v-model="fleet.sortKey.value"><option value="online">在线状态</option><option value="name">名称</option><option value="coreVersion">Core 版本</option><option value="clients">客户端数</option><option value="rate">在线率</option><option value="lastSeen">最后在线</option></select></label>
      <MdButton variant="outlined" @click="fleet.asc.value = !fleet.asc.value">{{ fleet.asc.value ? '升序 ↑' : '降序 ↓' }}</MdButton>
    </div>

    <div class="list">
      <MdCard v-for="d in fleet.rows.value" :key="d.deviceId" class="dev-card">
        <div class="dev-row">
          <span class="online-dot" :class="{ on: d.online }" :title="d.online ? '在线' : '离线'" />
          <div class="dev-main" @click="nav.go('device', d.deviceId)">
            <span class="dev-name">{{ d.name || d.deviceId }}</span>
            <span class="dev-meta">{{ d.deviceId }} · core {{ d.coreVersion || '?' }} · 最后在线 {{ fmtDateTime(d.lastSeen) }}</span>
          </div>
          <div class="dev-actions">
            <MdButton variant="tonal" @click="nav.go('device', d.deviceId)">详情</MdButton>
            <MdButton variant="text" @click="fleet.unclaim(d.deviceId)">解绑</MdButton>
          </div>
        </div>
      </MdCard>
      <EmptyState v-if="fleet.loaded.value && devices.devices.length > 0 && fleet.rows.value.length === 0" icon="search" title="没有匹配的设备" hint="试试其他关键词或状态。" />
      <EmptyState
        v-if="fleet.loaded.value && !devices.loading && devices.devices.length === 0"
        icon="devices"
        title="还没有设备"
        hint="点击「认领设备」，输入设备 ID 与认领码（真机请在局域网设备面板的 Cloud 设置中查看；模拟器见启动输出）。"
        action-text="认领设备"
        @action="showClaim = true"
      />
    </div>

    <div v-if="showClaim" class="scrim" @click.self="showClaim = false">
      <MdCard title="认领设备" class="dialog">
        <label class="lbl">设备 ID</label>
        <input v-model="claimId" type="text" spellcheck="false" placeholder="nya-xxxx" />
        <label class="lbl top-gap">认领码</label>
        <input v-model="claimCode" type="text" spellcheck="false" inputmode="numeric" placeholder="6 位数字" @keyup.enter="doClaim" />
        <p v-if="devices.lastError" class="err top-gap">{{ devices.lastError }}</p>
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
.page { padding: 22px; max-width: 960px; margin: 0 auto; }
.head { display: flex; justify-content: space-between; align-items: center; gap: 10px; margin-bottom: 18px; flex-wrap: wrap; }
.title { font-size: 22px; }
.head-actions { display: flex; gap: 10px; }
.list { display: flex; flex-direction: column; gap: 12px; }
.filters { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin-bottom: 20px; }
.filters input { flex: 1; min-width: min(100%,200px); }
.filters input,.filters select { padding: 10px 12px; border: 1px solid var(--md-outline-variant); border-radius: var(--radius-s); background: var(--md-surface-container); color: var(--md-on-surface); }
.dev-row { display: flex; align-items: center; gap: 14px; }
.online-dot { width: 10px; height: 10px; border-radius: 50%; background: var(--md-outline); flex: none; }
.online-dot.on { background: var(--md-success); box-shadow: 0 0 8px var(--md-success); }
.dev-main { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px; cursor: pointer; }
.dev-name { font: 600 15px var(--font-body); color: var(--md-on-surface); }
.dev-meta { font-size: 12px; color: var(--md-on-surface-variant); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dev-actions { display: flex; gap: 8px; flex: none; }
.err { color: var(--md-error); font-size: 13px; margin: 0 0 12px; }
.scrim { position: fixed; inset: 0; background: var(--md-scrim); display: flex; align-items: center; justify-content: center; z-index: 10; }
.dialog { width: min(400px, 90vw); }
.lbl { display: block; font-size: 12.5px; color: var(--md-on-surface-variant); margin-bottom: 8px; letter-spacing: 1px; }
.top-gap { margin-top: 14px; }
.row { margin-top: 18px; display: flex; justify-content: flex-end; gap: 10px; }
@media (max-width: 640px) {
  .dev-row { flex-wrap: wrap; }
  .dev-actions { width: 100%; justify-content: flex-end; }
}
</style>
