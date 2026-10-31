<script setup lang="ts">
/* Nebula devices: searchable list with a 30-day presence strip per device
 * (daily onlineSeconds / 86400 from GET /devices/{id}/stats), a claim
 * dialog with a 6-cell code input (POST /devices/claim), and the detail
 * drawer (nav.page === 'device'). */
import { computed, onMounted, ref } from 'vue';
import { EmptyState, Skeleton, UiIcon } from '@nyabula/ui';
import { useFleet } from '../../logic/useFleet';
import { useCodeCells } from '../../logic/useCodeCells';
import { fmtAgo } from '../../logic/format';
import NebulaDeviceDrawer from './NebulaDeviceDrawer.vue';

const fleet = useFleet();
const { devices, stats, nav } = fleet;
onMounted(() => void fleet.refresh());

const FILTERS = [
  { id: 'all', label: '全部' },
  { id: 'on', label: '在线' },
  { id: 'off', label: '离线' },
] as const;
const fIdx = computed(() => FILTERS.findIndex((f) => f.id === fleet.filter.value));

function strip(id: string): number[] {
  return stats.series(id, 30).map((d) => Math.min(1, d.onlineSeconds / 86400));
}

/* ---- claim ---- */
const claimOpen = ref(false);
const claimId = ref('');
const code = useCodeCells(6);
const claimState = ref<'idle' | 'busy' | 'ok'>('idle');
const claimErr = ref<string | null>(null);
function openClaim() {
  claimOpen.value = true;
  claimId.value = '';
  code.reset();
  claimState.value = 'idle';
  claimErr.value = null;
}
const canClaim = computed(() => claimId.value.trim().length > 0 && code.complete.value && claimState.value === 'idle');
async function claim() {
  if (!canClaim.value) return;
  claimState.value = 'busy';
  claimErr.value = null;
  const ok = await fleet.claim(claimId.value, code.value.value);
  if (ok) {
    claimState.value = 'ok';
    window.setTimeout(() => (claimOpen.value = false), 900);
  } else {
    claimState.value = 'idle';
    claimErr.value = devices.lastError ?? '认领失败';
  }
}
</script>

<template>
  <div class="dv">
    <header class="hd nb-in">
      <div>
        <p class="nb-kicker">{{ fleet.onlineCount.value }} 在线 · 共 {{ devices.devices.length }} 台</p>
        <h1 class="nb-title">设备</h1>
      </div>
      <div class="hd-a">
        <button class="nb-btn" title="刷新" :disabled="devices.loading" @click="fleet.refresh()"><UiIcon name="refresh" :size="17" :class="{ spin: devices.loading }" /></button>
        <button class="nb-btn primary" @click="openClaim"><UiIcon name="add" :size="18" /> 认领设备</button>
      </div>
    </header>

    <div class="tools nb-in" style="--d: 80ms">
      <label class="search"><UiIcon name="search" :size="17" /><input v-model="fleet.q.value" class="nb-input" placeholder="搜索名称、ID、版本" /></label>
      <div class="nb-seg seg">
        <span class="nb-seg-light" :style="{ width: 'calc((100% - 8px) / 3)', transform: `translateX(${fIdx * 100}%)` }" />
        <button v-for="f in FILTERS" :key="f.id" :class="{ on: fleet.filter.value === f.id }" @click="fleet.filter.value = f.id">{{ f.label }}</button>
      </div>
    </div>

    <p v-if="devices.lastError && !claimOpen" class="nb-err"><UiIcon name="error" :size="15" /> {{ devices.lastError }}</p>

    <div class="table nb-glass nb-in" style="--d: 140ms">
      <div class="th"><span>设备</span><span>版本</span><span>客户端</span><span>30 天在线</span><span /></div>
      <Skeleton v-if="!fleet.loaded.value" :lines="4" />
      <TransitionGroup v-else name="rows" tag="div" class="tb">
        <button v-for="d in fleet.rows.value" :key="d.deviceId" class="tr" :class="{ sel: nav.deviceId === d.deviceId }" @click="nav.go('device', d.deviceId)">
          <span class="c-name">
            <span class="nb-dot" :class="{ on: d.online }" />
            <span><b>{{ d.name || d.deviceId }}</b><small><span class="nb-mono">{{ d.deviceId }}</span> · {{ d.online ? '在线' : fmtAgo(d.lastSeen) }}</small></span>
          </span>
          <span class="nb-mono ver">{{ d.coreVersion || '—' }}</span>
          <span class="cl">{{ d.clients ?? '—' }}</span>
          <span class="heat" :title="d.rate !== null ? `30 天在线率 ${(d.rate * 100).toFixed(1)}%` : ''">
            <i v-for="(h, i) in strip(d.deviceId)" :key="i" :style="{ opacity: h > 0 ? 0.25 + h * 0.75 : 1 }" :class="{ lit: h > 0 }" />
          </span>
          <UiIcon name="chevron_right" :size="18" class="chev" />
        </button>
      </TransitionGroup>
      <EmptyState
        v-if="fleet.loaded.value && !devices.devices.length"
        icon="devices"
        title="还没有设备"
        hint="点击「认领设备」，输入设备 ID 与认领码（真机请在局域网设备面板的 Cloud 设置中查看；模拟器见启动输出）。"
        action-text="认领设备"
        @action="openClaim"
      />
      <p v-else-if="fleet.loaded.value && !fleet.rows.value.length" class="nb-empty">没有匹配的设备</p>
    </div>

    <NebulaDeviceDrawer />

    <Teleport to="body">
      <div class="nbc-portal">
        <Transition name="nb-fade"><div v-if="claimOpen" class="scrim" @click="claimOpen = false" /></Transition>
        <Transition name="modal">
          <form v-if="claimOpen" class="modal nb-glass" role="dialog" aria-label="认领设备" @submit.prevent="claim">
            <div class="m-ico" :class="claimState"><UiIcon :name="claimState === 'ok' ? 'check' : 'qr_code'" :size="28" /></div>
            <h2 class="nb-title">{{ claimState === 'ok' ? '认领成功' : '认领设备' }}</h2>
            <p class="m-sub">设备 ID 与 6 位认领码：真机请在局域网设备面板的 Cloud 设置中查看；模拟器见启动输出</p>
            <input v-model="claimId" class="nb-input" placeholder="设备 ID，如 nya-a44a011c" spellcheck="false" autofocus />
            <div class="cells">
              <input
                v-for="(c, i) in code.cells.value"
                :key="i"
                :ref="code.bind(i)"
                :value="c"
                class="cell"
                :class="{ filled: !!c }"
                inputmode="numeric"
                autocomplete="one-time-code"
                maxlength="6"
                :aria-label="`认领码第 ${i + 1} 位`"
                @input="code.onInput(i, $event)"
                @keydown="code.onKey(i, $event)"
              />
            </div>
            <Transition name="nb-fade"><p v-if="claimErr" class="nb-err"><UiIcon name="error" :size="15" /> {{ claimErr }}</p></Transition>
            <div class="m-actions">
              <button type="button" class="nb-btn" @click="claimOpen = false">取消</button>
              <button type="submit" class="nb-btn primary" :disabled="!canClaim">{{ claimState === 'busy' ? '认领中…' : '认领' }}</button>
            </div>
          </form>
        </Transition>
      </div>
    </Teleport>
  </div>
</template>

<style scoped>
.dv { display: flex; flex-direction: column; gap: 16px; }
.hd { display: flex; align-items: flex-end; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.hd h1 { font-size: clamp(28px, 3.4vw, 44px); margin: 6px 0 0; }
.hd-a { display: flex; gap: 8px; }
.spin { animation: sp 0.8s linear infinite; }
@keyframes sp { to { transform: rotate(360deg); } }
.tools { display: flex; gap: 12px; flex-wrap: wrap; }
.search { position: relative; flex: 1; min-width: 200px; color: var(--dim); }
.search :deep(.ui-icon) { position: absolute; left: 14px; top: 50%; transform: translateY(-50%); }
.search .nb-input { padding-left: 40px; border-radius: 999px; }
.seg { min-width: 210px; }
.table { padding: 8px; overflow: hidden; }
.th, .tr { display: grid; grid-template-columns: minmax(180px, 2fr) 96px 64px minmax(150px, 1.6fr) 24px; align-items: center; gap: 14px; padding: 12px 14px; }
.th { color: var(--sub); font-size: 12px; }
.tr { width: 100%; border: none; background: transparent; color: var(--fg); text-align: left; cursor: pointer; border-radius: 16px; font: inherit; transition: background 0.2s, transform 0.4s var(--ease); }
.tr:hover { background: var(--wash); }
.tr.sel { background: rgba(var(--acc-rgb), 0.1); }
.tr:active { transform: scale(0.995); }
.c-name { display: flex; align-items: center; gap: 12px; min-width: 0; }
.c-name > span:last-child { display: flex; flex-direction: column; min-width: 0; }
.c-name b { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.c-name small { color: var(--sub); font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.ver { font-size: 12.5px; color: var(--sub); }
.heat { display: flex; gap: 1.5px; height: 20px; align-items: stretch; }
.heat i { flex: 1; border-radius: 2px; background: var(--wash-2); }
.heat i.lit { background: var(--acc); }
.chev { color: var(--dim); }
.tb { position: relative; }
.rows-enter-active, .rows-leave-active { transition: opacity 0.35s var(--ease), transform 0.45s var(--ease); }
.rows-enter-from, .rows-leave-to { opacity: 0; transform: translateX(-12px); }
.rows-leave-active { position: absolute; width: calc(100% - 0px); }
.rows-move { transition: transform 0.45s var(--ease); }

.scrim { position: fixed; inset: 0; z-index: 150; background: color-mix(in srgb, var(--md-scrim) 100%, transparent); backdrop-filter: blur(6px); }
.modal { position: fixed; z-index: 170; left: 50%; top: 50%; translate: -50% -50%; width: min(420px, calc(100vw - 28px)); padding: 28px; background: var(--panel-strong); border-radius: 30px; display: flex; flex-direction: column; align-items: center; gap: 12px; text-align: center; }
.modal-enter-active { transition: transform 0.55s var(--spring), opacity 0.3s; }
.modal-leave-active { transition: transform 0.25s ease, opacity 0.2s; }
.modal-enter-from { transform: scale(0.9) translateY(20px); opacity: 0; }
.modal-leave-to { transform: scale(0.96); opacity: 0; }
.m-ico { width: 64px; height: 64px; border-radius: 22px; display: grid; place-items: center; background: color-mix(in srgb, var(--md-tertiary) 18%, transparent); color: var(--md-tertiary); transition: all 0.5s var(--spring); }
.m-ico.busy { animation: wob 1s ease-in-out infinite; }
@keyframes wob { 50% { transform: scale(0.9) rotate(-6deg); } }
.m-ico.ok { background: var(--acc); color: var(--md-on-primary); transform: scale(1.1); box-shadow: 0 0 40px -6px rgba(var(--acc-rgb), 0.9); }
.modal h2 { margin: 0; font-size: 26px; }
.m-sub { color: var(--sub); margin: 0 0 6px; font-size: 12.5px; line-height: 1.6; }
.cells { display: grid; grid-template-columns: repeat(6, 1fr); gap: 8px; width: 100%; }
.cell { width: 100%; min-width: 0; aspect-ratio: 0.85; text-align: center; font: 600 24px ui-monospace, monospace; color: var(--fg); background: var(--wash); border: 1px solid var(--line); border-radius: 14px; outline: none; transition: border-color 0.2s, transform 0.3s var(--spring), box-shadow 0.3s, background 0.2s; }
.cell:focus { border-color: rgba(var(--acc-rgb), 0.7); box-shadow: 0 0 0 4px rgba(var(--acc-rgb), 0.14); transform: translateY(-2px); }
.cell.filled { background: rgba(var(--acc-rgb), 0.1); }
.m-actions { display: flex; gap: 10px; margin-top: 8px; }
@media (max-width: 900px) {
  .th { display: none; }
  .tr { grid-template-columns: minmax(0, 1fr) auto 24px; }
  .tr > .cl { display: none; }
  .tr .heat { grid-column: 1 / -1; order: 5; }
}
@media (max-width: 480px) { .seg { min-width: 0; width: 100%; } }
</style>
