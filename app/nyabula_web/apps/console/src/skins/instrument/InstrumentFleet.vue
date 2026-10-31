<script setup lang="ts">
/* Device fleet: dense sortable / filterable table (GET /devices + per-
 * device stats for clients and 30-day online rate). Rows expand inline
 * into a strip with the 30-day online columns, the remote-takeover key
 * and a hold-to-unclaim danger key (DELETE /devices/{id}). "认领设备"
 * opens the claim dialog with a 6-cell code (POST /devices/claim). */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { EmptyState, UiIcon } from '@nyabula/ui';
import LedBar from '../../components/LedBar.vue';
import { useFleet, type FleetSort } from '../../logic/useFleet';
import { useCodeCells } from '../../logic/useCodeCells';
import { fmtAgo, fmtDate, fmtHours } from '../../logic/format';

const fleet = useFleet();
const { devices, stats, nav } = fleet;
const open = ref<string | null>(null);
onMounted(() => void fleet.refresh());

const COLS: { k: FleetSort; label: string }[] = [
  { k: 'online', label: 'ST' },
  { k: 'name', label: '名称 / ID' },
  { k: 'coreVersion', label: '版本' },
  { k: 'clients', label: '客户端' },
  { k: 'rate', label: '在线率 30D' },
  { k: 'lastSeen', label: '最后在线' },
];

function daily(id: string) {
  return stats.series(id, 30);
}

/* ---- hold to unclaim ---- */
const holding = ref<string | null>(null);
let holdT = 0;
function holdStart(id: string) {
  holding.value = id;
  holdT = window.setTimeout(async () => {
    holding.value = null;
    const ok = await fleet.unclaim(id, false);
    if (ok && open.value === id) open.value = null;
  }, 1500);
}
function holdEnd() {
  clearTimeout(holdT);
  holding.value = null;
}
onBeforeUnmount(() => clearTimeout(holdT));

/* ---- claim dialog ---- */
const claimOpen = ref(false);
const claimId = ref('');
const code = useCodeCells(6);
const claimErr = ref('');
const claiming = ref(false);
function openClaim() {
  claimOpen.value = true;
  claimId.value = '';
  code.reset();
  claimErr.value = '';
}
async function claim() {
  claimErr.value = '';
  if (!claimId.value.trim()) return void (claimErr.value = 'ERR  请输入设备 ID');
  if (!code.complete.value) return void (claimErr.value = 'ERR  请输入 6 位认领码');
  claiming.value = true;
  const ok = await fleet.claim(claimId.value, code.value.value);
  claiming.value = false;
  if (ok) {
    open.value = claimId.value.trim();
    claimOpen.value = false;
  } else claimErr.value = `ERR  ${devices.lastError ?? '认领失败'}`;
}
const counts = computed(() => ({ all: devices.devices.length, on: fleet.onlineCount.value, off: devices.devices.length - fleet.onlineCount.value }));
</script>

<template>
  <div class="fleet ins-boot">
    <header class="tools">
      <div class="ins-seg">
        <button :class="{ on: fleet.filter.value === 'all' }" @click="fleet.filter.value = 'all'">ALL {{ counts.all }}</button>
        <button :class="{ on: fleet.filter.value === 'on' }" @click="fleet.filter.value = 'on'">ONLINE {{ counts.on }}</button>
        <button :class="{ on: fleet.filter.value === 'off' }" @click="fleet.filter.value = 'off'">OFFLINE {{ counts.off }}</button>
      </div>
      <label class="search"><UiIcon name="search" :size="16" /><input v-model="fleet.q.value" class="ins-input" placeholder="搜索名称 / ID / 版本" /></label>
      <button class="ins-key" :disabled="devices.loading" @click="fleet.refresh()"><UiIcon name="refresh" :size="15" /> 刷新</button>
      <button class="ins-key solid" @click="openClaim"><UiIcon name="add" :size="16" /> 认领设备</button>
    </header>
    <p v-if="devices.lastError && !claimOpen" class="ins-err">ERR  {{ devices.lastError }}</p>

    <div class="tbl">
      <div class="tr th">
        <button v-for="c in COLS" :key="c.k" class="ins-label thb" :class="{ on: fleet.sortKey.value === c.k }" @click="fleet.sortBy(c.k)">
          {{ c.label }}<span class="arr">{{ fleet.sortKey.value === c.k ? (fleet.asc.value ? '▲' : '▼') : '' }}</span>
        </button>
      </div>
      <TransitionGroup name="row">
        <div v-for="d in fleet.rows.value" :key="d.deviceId" class="grp">
          <div class="tr" :class="{ open: open === d.deviceId }" @click="open = open === d.deviceId ? null : d.deviceId">
            <span><span class="ins-led" :class="{ on: d.online }" /></span>
            <span class="nm"><b>{{ d.name || d.deviceId }}</b><span class="ins-label id">{{ d.deviceId }}</span></span>
            <span class="ins-num">{{ d.coreVersion || '--' }}</span>
            <span class="ins-num">{{ d.clients ?? '--' }}</span>
            <span class="up"><LedBar :value="d.rate ?? 0" :segments="14" :warn-below="0.2" /><span class="ins-num">{{ d.rate !== null ? (d.rate * 100).toFixed(1) : '--' }}</span></span>
            <span class="ins-dim ls">{{ d.online ? 'NOW' : fmtAgo(d.lastSeen) }}</span>
          </div>
          <Transition name="exp">
            <div v-if="open === d.deviceId" class="det">
              <div class="det-c">
                <span class="ins-label">ONLINE HOURS · 30D</span>
                <div class="bars">
                  <i
                    v-for="(x, i) in daily(d.deviceId)"
                    :key="x.date"
                    :title="`${x.date} · ${fmtHours(x.onlineSeconds)}`"
                    :style="{ transform: `scaleY(${Math.max(0.03, x.onlineSeconds / 86400)})`, animationDelay: i * 12 + 'ms' }"
                  />
                </div>
                <div class="det-kv">
                  <span><span class="ins-label">CLAIMED</span> <span class="ins-num">{{ fmtDate(d.claimedAt) }}</span></span>
                  <span><span class="ins-label">CORE</span> <span class="ins-num">{{ d.coreVersion || '--' }}</span></span>
                  <span><span class="ins-label">CLIENTS</span> <span class="ins-num">{{ d.clients ?? '--' }}</span></span>
                </div>
              </div>
              <div class="det-a">
                <button class="ins-key on" @click.stop="nav.go('device', d.deviceId)">远程接管 REMOTE <UiIcon name="arrow_forward" :size="14" /></button>
                <button
                  class="ins-key danger hold"
                  :class="{ holding: holding === d.deviceId }"
                  @click.stop
                  @pointerdown="holdStart(d.deviceId)"
                  @pointerup="holdEnd"
                  @pointerleave="holdEnd"
                  @pointercancel="holdEnd"
                  @keydown.enter.prevent="fleet.unclaim(d.deviceId)"
                >
                  <span class="fill" />
                  <span class="hl">{{ holding === d.deviceId ? '松开取消…' : '按住 1.5s 解绑' }}</span>
                </button>
              </div>
            </div>
          </Transition>
        </div>
      </TransitionGroup>
      <EmptyState
        v-if="fleet.loaded.value && !devices.devices.length"
        icon="devices"
        title="还没有设备"
        hint="点击「认领设备」，输入设备 ID 与认领码（真机请在局域网设备面板的 Cloud 设置中查看；模拟器见启动输出）。"
        action-text="认领设备"
        @action="openClaim"
      />
      <p v-else-if="fleet.loaded.value && !fleet.rows.value.length" class="empty ins-label">NO MATCH · 没有符合条件的设备</p>
    </div>

    <Teleport to="body">
      <div class="insc-portal">
        <Transition name="fade"><div v-if="claimOpen" class="scrim" @click="claimOpen = false" /></Transition>
        <Transition name="dlg">
          <form v-if="claimOpen" class="dialog ins-panel" role="dialog" aria-label="认领设备" @submit.prevent="claim">
            <div class="ins-panel-head"><span class="ins-label"><span class="ins-idx">+</span> CLAIM DEVICE</span><button type="button" class="ins-key x" aria-label="关闭" @click="claimOpen = false"><UiIcon name="close" :size="15" /></button></div>
            <h3>认领设备</h3>
            <p class="ins-dim hint">设备 ID 与 6 位认领码：真机请在局域网设备面板的 Cloud 设置中查看；模拟器见启动输出。</p>
            <label class="fld"><span class="ins-label">DEVICE ID</span><input v-model="claimId" class="ins-input" placeholder="nya-xxxxxxxx" spellcheck="false" autofocus /></label>
            <div class="fld">
              <span class="ins-label">CLAIM CODE</span>
              <div class="code">
                <input
                  v-for="(c, i) in code.cells.value"
                  :key="i"
                  :ref="code.bind(i)"
                  :value="c"
                  class="cell"
                  :class="{ fill: c }"
                  inputmode="numeric"
                  autocomplete="one-time-code"
                  maxlength="6"
                  :aria-label="`认领码第 ${i + 1} 位`"
                  @input="code.onInput(i, $event)"
                  @keydown="code.onKey(i, $event)"
                />
              </div>
            </div>
            <p v-if="claimErr" class="ins-err">{{ claimErr }}</p>
            <button class="ins-key solid go" :disabled="claiming">{{ claiming ? 'VERIFYING…' : '认领 CLAIM ↵' }}</button>
          </form>
        </Transition>
      </div>
    </Teleport>
  </div>
</template>

<style scoped>
.fleet { padding: 16px 20px 28px; max-width: 1480px; display: flex; flex-direction: column; gap: 12px; }
.tools { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.search { position: relative; flex: 1; min-width: 180px; max-width: 360px; color: var(--c-dim); }
.search :deep(.ui-icon) { position: absolute; left: 11px; top: 11px; }
.search .ins-input { padding-left: 34px; }
.tools .ins-key.solid { margin-left: auto; }
.tbl { border: 1px solid var(--c-line); border-radius: 4px; background: var(--c-panel); overflow: hidden; position: relative; }
.tr { display: grid; grid-template-columns: 40px minmax(0, 2fr) 110px 70px minmax(150px, 1.4fr) 100px; align-items: center; gap: 12px; padding: 11px 16px; border-top: 1px solid var(--c-line); cursor: pointer; transition: background 120ms; }
.tr:hover:not(.th) { background: var(--c-panel-2); }
.tr.open { background: var(--c-panel-2); }
.th { border-top: none; cursor: default; background: var(--md-surface-dim); padding: 9px 16px; }
.thb { background: none; border: none; padding: 0; text-align: left; cursor: pointer; display: inline-flex; gap: 4px; }
.thb:hover, .thb.on { color: var(--c-fg); }
.arr { color: var(--c-sig); font-size: 8px; }
.nm { display: flex; flex-direction: column; gap: 3px; min-width: 0; }
.nm b { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.id { text-transform: none; letter-spacing: 0.04em; }
.up { display: flex; align-items: center; gap: 10px; }
.up .ins-num { width: 40px; text-align: right; font-size: 12px; }
.ls { font: 600 12px var(--c-mono); }
.row-move { transition: transform 260ms var(--c-ease); }
.row-enter-active { transition: opacity 220ms, transform 220ms var(--c-ease); }
.row-enter-from { opacity: 0; transform: translateX(-8px); }
.row-leave-active { position: absolute; width: 100%; transition: opacity 180ms, transform 180ms; }
.row-leave-to { opacity: 0; transform: translateX(16px); }
.det { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 18px; padding: 14px 16px 16px 68px; background: var(--c-panel-2); border-top: 1px dashed var(--c-line); transform-origin: top; }
.det-c { display: flex; flex-direction: column; gap: 10px; min-width: 0; }
.bars { display: flex; align-items: flex-end; gap: 2px; height: 64px; }
.bars i { flex: 1; height: 100%; background: var(--c-sig); transform-origin: bottom; border-radius: 1px; animation: grow 420ms var(--c-ease) both; }
@keyframes grow { from { transform: scaleY(0); } }
.det-kv { display: flex; gap: 18px; flex-wrap: wrap; font-size: 12px; }
.det-a { display: flex; flex-direction: column; gap: 8px; justify-content: flex-end; }
.hold { overflow: hidden; min-width: 160px; touch-action: none; }
.hold .fill { position: absolute; inset: 0; background: var(--c-warn); transform: scaleX(0); transform-origin: left; transition: transform 200ms ease-out; }
.hold.holding .fill { transform: scaleX(1); transition: transform 1.5s linear; }
.hold .hl { position: relative; }
.hold.holding { color: var(--md-on-error); }
.exp-enter-active { transition: opacity 200ms, transform 240ms var(--c-ease); }
.exp-leave-active { transition: opacity 100ms; }
.exp-enter-from { opacity: 0; transform: scaleY(0.85); }
.exp-leave-to { opacity: 0; }
.empty { padding: 28px; text-align: center; margin: 0; }
.scrim { position: fixed; inset: 0; z-index: 150; background: var(--md-scrim); }
.dialog { position: fixed; z-index: 151; left: 50%; top: 50%; width: min(440px, calc(100vw - 32px)); margin: 0; translate: -50% -50%; display: flex; flex-direction: column; gap: 14px; padding: 18px; background: var(--c-bg); border-color: var(--c-line-2); box-shadow: 0 30px 80px rgba(0, 0, 0, 0.45); }
.dialog h3 { margin: 0; font: 400 26px var(--font-title); }
.hint { margin: 0; font-size: 13px; line-height: 1.6; }
.x { width: 34px; min-height: 30px; padding: 0; }
.fld { display: flex; flex-direction: column; gap: 7px; }
.code { display: grid; grid-template-columns: repeat(6, minmax(0, 1fr)); gap: 6px; }
.cell { min-width: 0; height: 54px; text-align: center; background: var(--c-panel); color: var(--c-fg); border: 1px solid var(--c-line-2); border-bottom-width: 3px; border-radius: 3px; font: 600 22px var(--c-mono); outline: none; transition: border-color 140ms, transform 140ms var(--c-ease); }
.cell:focus { border-color: var(--c-sig); }
.cell.fill { border-color: var(--c-fg); transform: translateY(-1px); }
.go { min-height: 44px; }
.fade-enter-active, .fade-leave-active { transition: opacity 180ms; }
.fade-enter-from, .fade-leave-to { opacity: 0; }
.dlg-enter-active { transition: opacity 200ms, transform 260ms var(--c-ease); }
.dlg-leave-active { transition: opacity 120ms, transform 120ms; }
.dlg-enter-from { opacity: 0; transform: translateY(10px) scale(0.98); }
.dlg-leave-to { opacity: 0; transform: scale(0.98); }
@media (max-width: 1180px) {
  .tr { grid-template-columns: 30px minmax(0, 2fr) 96px minmax(120px, 1.2fr) 90px; }
  .tr > :nth-child(4) { display: none; }
  .det { grid-template-columns: minmax(0, 1fr); padding-left: 16px; }
  .det-a { flex-direction: row; flex-wrap: wrap; justify-content: flex-start; }
}
@media (max-width: 720px) {
  .fleet { padding: 12px; }
  .tools .ins-key.solid { margin-left: 0; }
  .search { max-width: none; flex-basis: 100%; }
  .th { display: none; }
  .tr { grid-template-columns: 18px minmax(0, 1fr) auto; grid-template-areas: 'l n v' 'l u u'; row-gap: 8px; }
  .tr > :nth-child(1) { grid-area: l; align-self: start; padding-top: 4px; }
  .tr > .nm { grid-area: n; }
  .tr > :nth-child(3) { grid-area: v; }
  .tr > .up { grid-area: u; }
  .tr > :nth-child(6) { display: none; }
  .det-a { flex-direction: column; }
}
</style>
