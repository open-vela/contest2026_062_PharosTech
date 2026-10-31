<script setup lang="ts">
import PluginIcon from '../../../components/PluginIcon.vue';
import { roleLabel } from '@nyabula/ui';
/* Instrument home: a main-control panel grid built on the shared home
 * logic (useHomePage). Covers everything the classic variants show:
 * device state/role/core/transport, battery, WiFi, uptime, cloud relay,
 * live eye preview with pairing + quick expressions, quick actions,
 * attention items, plugin summary, Nyabot quick ask, widgets, and the
 * activity feed (ContextSlot: desktop side panel, inline elsewhere).
 * Readouts without a data source are shown as "--", never invented. */
import { computed, inject, ref } from 'vue';
import { useRouter } from 'vue-router';
import { EyeCanvas, PairOverlay, Skeleton, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../../components/ContextSlot.vue';
import WidgetGrid from '../../../components/WidgetGrid.vue';
import type { useFormFactor } from '../../../composables/useFormFactor';
import { useHomePage } from '../../../views/home/home.logic';
import InsPanel from '../parts/InsPanel.vue';
import EyeMirrorCard from '../../../views/eye/EyeMirrorCard.vue';
import EyeMonitor from '../parts/EyeMonitor.vue';
import DotMatrix from '../parts/DotMatrix.vue';
import LedBar from '../parts/LedBar.vue';

const page = useHomePage();
const { session, eye, go, info, cloud, infoTask, battery, wifi, bars, deviceName, coreVersion, stateLabel, stateTone } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const router = useRouter();
const layout = computed(() => (ff.formFactor.value === 'tablet' && ff.orientation.value === 'portrait' ? 'phone' : ff.formFactor.value));
const feedLimit = computed(() => (ff.formFactor.value === 'desktop' ? 12 : ff.formFactor.value === 'tablet' ? 6 : 5));
const idx = (i: number) => String(i + 1).padStart(2, '0');

/* ---- readouts ---- */
const battText = computed(() => (battery.value.external ? 'DC IN' : battery.value.level !== null ? `${battery.value.level}%` : '--%'));
const battTone = computed<'ok' | 'warn' | 'err'>(() =>
  battery.value.level !== null && battery.value.level <= 20 && !battery.value.charging ? 'err' : 'ok',
);
/** Compact uptime for the dot-matrix: "3D07H" or "07:42" (h:m). */
const uptimeText = computed(() => {
  const s = info.value?.uptime;
  if (typeof s !== 'number') return '--:--';
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const p = (n: number) => String(n).padStart(2, '0');
  return d > 0 ? `${d}D${p(h)}H` : `${p(h)}:${p(m)}`;
});
const cloudState = computed<{ tone: 'ok' | 'warn' | 'off'; text: string }>(() => {
  if (!cloud.value) return { tone: 'off', text: '—' };
  if (cloud.value.state === 'unsupported') return { tone: 'off', text: '暂不支持' };
  if (cloud.value.state === 'connecting') return { tone: 'warn', text: '连接中' };
  if (!cloud.value.enabled) return { tone: 'off', text: '未启用' };
  return cloud.value.state === 'online' ? { tone: 'ok', text: '已连通' } : { tone: 'warn', text: '未连通' };
});

/* ---- eye preview + pairing (mirrors EyePreviewCard) ---- */
const pairing = ref(false);
const pairError = ref<string | null>(null);
async function doPair(code: string) {
  pairing.value = true;
  pairError.value = null;
  try {
    await session.pair(code, 'Nyabula Web');
  } catch {
    pairError.value = session.lastError && /wrong pairing code|EPERM/i.test(session.lastError) ? '配对码错误' : session.lastError ?? '配对失败';
  } finally {
    pairing.value = false;
  }
}

/* ---- quick ask (mirrors AgentQuickAsk) ---- */
const ask = ref('');
const suggestions = ['今天天气怎么样？', '设个 10 分钟倒计时', '播放点音乐', '你现在心情如何？'];
function send(t = ask.value) {
  const q = t.trim();
  if (!q) return;
  void router.push({ name: 'agent', params: { key: session.deviceKey ?? '' }, query: { q } });
}
</script>

<template>
  <div class="ins-home" :class="`l-${layout}`">
    <!-- 01 DEVICE -->
    <InsPanel idx="01" code="DEVICE" label="设备" class="a-dev">
      <template #aside>
        <span class="state" :class="stateTone"><i class="led" />{{ stateLabel }}</span>
      </template>
      <div class="dev-id">
        <h2 class="dev-name">{{ deviceName }}</h2>
        <div class="dev-tags">
          <span v-if="session.role" class="tag">{{ roleLabel(session.role) }}</span>
          <span v-if="coreVersion" class="tag mono">CORE {{ coreVersion }}</span>
          <span v-if="session.transport === 'cloud'" class="tag info"><UiIcon name="cloud" :size="12" /> 云中继</span>
        </div>
      </div>
      <div class="gauges">
        <div class="gauge">
          <span class="g-key">{{ battery.external ? 'POWER · 已连接电源' : battery.charging ? 'BATT · 充电中' : 'BATT · 电量' }}</span>
          <DotMatrix :text="battText" :dot="5" :gap="1.8" :class="['g-num', battTone]" />
          <LedBar v-if="!battery.external" :value="(battery.level ?? 0) / 100" :segments="16" :tone="battTone" />
        </div>
        <div class="gauge">
          <span class="g-key">WIFI · RSSI</span>
          <div class="wifi">
            <span class="wbars" :title="wifi.rssi !== null ? wifi.rssi + ' dBm' : ''">
              <i v-for="i in 4" :key="i" :class="{ on: i <= bars }" :style="{ height: 5 + i * 5 + 'px' }" />
            </span>
            <span class="w-db mono">{{ wifi.rssi !== null ? wifi.rssi : '--' }}<small>dBm</small></span>
          </div>
          <span class="g-sub mono">{{ wifi.ssid ?? '—' }}</span>
        </div>
        <div class="gauge">
          <span class="g-key">UPTIME · 运行</span>
          <DotMatrix :text="uptimeText" :dot="5" :gap="1.8" class="g-num" />
          <span class="g-sub">{{ page.uptime.value }}</span>
        </div>
      </div>
      <div class="cloud-row">
        <span class="g-key">CLOUD · 云中继</span>
        <span class="cloud-val" :class="cloudState.tone"><i class="led" />{{ cloudState.text }}</span>
        <span v-if="cloud?.url" class="cloud-url mono">{{ cloud.url }}</span>
      </div>
      <Skeleton v-if="infoTask.busy.value && !info" :lines="2" />
    </InsPanel>

    <!-- 02 EYE -->
    <InsPanel idx="02" code="EYE" label="眼睛" class="a-eye">
      <template #aside>
        <button class="link" @click="go('eye')">完整控制 <UiIcon name="chevron_right" :size="14" /></button>
      </template>
      <div class="eye-box" @click="session.connected && go('eye')">
        <EyeMonitor :live="eye.ready">
          <EyeMirrorCard v-if="eye.ready && eye.nativeQr" embedded auto-start />
          <EyeCanvas
            v-else-if="eye.ready"
            class="eye-cv"
            :eye-state="eye.lastState"
            :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()"
            @look="eye.look"
          />
          <div v-else class="eye-ph">
            <UiIcon name="visibility" :size="30" />
            <span>{{ session.connected ? '眼睛服务未启动' : session.state === 'pairing-required' ? '等待配对' : '设备离线' }}</span>
          </div>
          <PairOverlay
            v-if="session.state === 'pairing-required' || (pairing && session.state === 'authenticating')"
            :busy="pairing"
            :error="pairError"
            @submit="doPair"
            @click.stop
          />
          <template #tl>MODE · {{ page.modeLabel.value }}</template>
          <template #tr><span v-if="page.sceneLabel.value">SCENE · {{ page.sceneLabel.value }}</span></template>
          <template #bl>{{ eye.activeMode }}</template>
          <template #br><span :class="{ rec: eye.ready }">{{ eye.ready ? '● LIVE' : '○ OFF' }}</span></template>
        </EyeMonitor>
      </div>
      <div class="mode-keys">
        <button
          v-for="m in page.quickModes"
          :key="m.id"
          class="mkey"
          :class="{ on: eye.activeMode === m.id && !eye.activeScene }"
          :disabled="!session.canControl || !eye.ready"
          @click="eye.setMode(m.id)"
        >
          <i class="led" />{{ m.label }}
        </button>
        <button v-if="eye.activeScene" class="mkey exit" @click="eye.setScene(null)"><UiIcon name="close" :size="13" /> 退出场景</button>
      </div>
    </InsPanel>

    <!-- 03 QUICK -->
    <InsPanel idx="03" code="QUICK" label="快捷操作" class="a-quick">
      <div class="qkeys">
        <button v-for="(a, i) in page.quickActions.value" :key="a.id" class="qkey" @click="a.run()">
          <span class="q-top"><span class="q-idx">{{ idx(i) }}</span><UiIcon :name="a.icon" :size="18" /></span>
          <span class="q-label">{{ a.label }}</span>
          <span v-if="a.hint" class="q-hint">{{ a.hint }}</span>
        </button>
      </div>
    </InsPanel>

    <!-- 04 ATTN -->
    <InsPanel idx="04" code="ATTN" label="需要关注" class="a-attn">
      <template #aside><span class="count mono">{{ String(page.attention.value.length).padStart(2, '0') }}</span></template>
      <div v-if="page.attention.value.length" class="attn">
        <button v-for="a in page.attention.value" :key="a.id" class="attn-row" :class="a.tone" @click="a.run()">
          <i class="led" />
          <UiIcon :name="a.icon" :size="18" />
          <span class="attn-body"><span class="attn-title">{{ a.title }}</span><span class="attn-sub">{{ a.sub }}</span></span>
          <UiIcon name="chevron_right" :size="16" />
        </button>
      </div>
      <div v-else class="all-good"><i class="led on" /><span>一切正常，没有待处理事项</span></div>
    </InsPanel>

    <!-- 05 PLUGINS -->
    <InsPanel idx="05" code="EXT" label="插件" class="a-plug">
      <template #aside>
        <button class="link" @click="go('plugins')">全部插件 <UiIcon name="chevron_right" :size="14" /></button>
      </template>
      <div class="plug-stats">
        <div><DotMatrix :text="String(page.pluginCount.value).padStart(2, '0')" :dot="4" :gap="1.5" /><span class="g-key">已安装</span></div>
        <div><DotMatrix :text="String(page.runningCount.value).padStart(2, '0')" :dot="4" :gap="1.5" class="sig" /><span class="g-key">运行中</span></div>
      </div>
      <div v-if="page.plugins.list.length" class="plug-chips">
        <button v-for="p in page.plugins.list.slice(0, 6)" :key="p.id" class="pkey" @click="go('plugin', { id: p.id })">
          <PluginIcon :plugin="p" :size="15" />{{ p.name }}
        </button>
      </div>
      <p v-else class="muted small">尚未安装插件。</p>
    </InsPanel>

    <!-- 06 ASK -->
    <InsPanel idx="06" code="NYABOT" label="问问 Nyabula" class="a-ask">
      <form class="cli" @submit.prevent="send()">
        <span class="prompt">&gt;</span>
        <input v-model="ask" class="cli-in" placeholder="问问 Nyabula…" aria-label="问问 Nyabula" />
        <button class="cli-send" type="submit" :disabled="!ask.trim()">发送 ↵</button>
      </form>
      <div class="sugg">
        <button v-for="s in suggestions" :key="s" class="skey" @click="send(s)">{{ s }}</button>
      </div>
    </InsPanel>

    <WidgetGrid :form-factor="ff.formFactor.value" class="a-wid" />

    <ContextSlot>
      <InsPanel idx="07" code="LOG" label="最近动态" class="a-log">
        <ul v-if="page.activity.value.length" class="log">
          <li v-for="a in page.activity.value.slice(0, feedLimit)" :key="a.id" class="log-row" :class="a.tone">
            <span class="log-t mono">{{ page.ago(a.at) }}</span>
            <i class="led" />
            <span class="log-text">{{ a.text }}</span>
          </li>
        </ul>
        <p v-else class="muted small">连接后这里会显示表情、场景与插件的实时变化。</p>
      </InsPanel>
    </ContextSlot>
  </div>
</template>

<style scoped>
.ins-home {
  display: grid;
  gap: 12px;
  padding: 16px 20px 40px;
  max-width: 1440px;
  margin: 0 auto;
  grid-template-columns: repeat(12, minmax(0, 1fr));
  grid-template-areas:
    'dev dev dev dev dev eye eye eye eye eye eye eye'
    'quick quick quick quick quick eye eye eye eye eye eye eye'
    'attn attn attn attn attn attn plug plug plug plug plug plug'
    'ask ask ask ask ask ask ask ask ask ask ask ask'
    'wid wid wid wid wid wid wid wid wid wid wid wid';
}
.l-tablet {
  padding: 14px 16px 32px;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  grid-template-areas: 'dev eye' 'quick eye' 'attn plug' 'ask ask' 'log log' 'wid wid';
}
.l-phone {
  padding: 10px 12px 28px;
  grid-template-columns: minmax(0, 1fr);
  grid-template-areas: 'dev' 'eye' 'quick' 'attn' 'ask' 'plug' 'log' 'wid';
}
.a-dev { grid-area: dev; }
.a-eye { grid-area: eye; }
.a-quick { grid-area: quick; }
.a-attn { grid-area: attn; }
.a-plug { grid-area: plug; }
.a-ask { grid-area: ask; }
.a-wid { grid-area: wid; }
.a-log { grid-area: log; }
.ins-home > :nth-child(2) { animation-delay: 40ms; }
.ins-home > :nth-child(3) { animation-delay: 80ms; }
.ins-home > :nth-child(4) { animation-delay: 120ms; }
.ins-home > :nth-child(5) { animation-delay: 160ms; }
.ins-home > :nth-child(6) { animation-delay: 200ms; }

.mono { font-family: var(--ins-mono); }
.small { font-size: 12.5px; margin: 0; }
.led { display: inline-block; width: 7px; height: 7px; border-radius: 50%; background: var(--md-outline); flex: none; }
.ok > .led, .led.on { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.warn > .led { background: var(--md-warning); box-shadow: 0 0 6px var(--md-warning); }
.err > .led { background: var(--md-error); box-shadow: 0 0 6px var(--md-error); }
.info > .led { background: var(--md-tertiary); }
.link { display: inline-flex; align-items: center; gap: 2px; border: none; background: transparent; color: var(--md-primary); font: 600 11.5px var(--ins-mono); letter-spacing: 0.06em; cursor: pointer; padding: 0; }
.g-key { font: 600 10px var(--ins-mono); letter-spacing: 0.14em; color: var(--md-on-surface-variant); text-transform: uppercase; }
.g-sub { font-size: 12px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

/* device */
.state { display: inline-flex; align-items: center; gap: 6px; font: 600 11px var(--ins-mono); letter-spacing: 0.08em; color: var(--md-on-surface); }
.state.ok { color: var(--md-primary); }
.state.warn { color: var(--md-warning); }
.state.err { color: var(--md-error); }
.state.warn .led { animation: blink 0.9s steps(2, jump-none) infinite; }
.dev-id { display: flex; flex-direction: column; gap: 8px; min-width: 0; }
.dev-name { font: 400 22px var(--font-title); color: var(--md-on-surface); margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.dev-tags { display: flex; flex-wrap: wrap; gap: 6px; }
.gauges { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 1px; background: var(--md-outline-variant); border: 1px solid var(--md-outline-variant); }
.l-phone .gauges { grid-template-columns: repeat(3, minmax(0, 1fr)); }
.gauge { display: flex; flex-direction: column; gap: 8px; padding: 10px; background: var(--md-surface-container); min-width: 0; }
.g-num { color: var(--md-on-surface); max-width: 100%; }
.g-num.err { color: var(--md-error); }
.wifi { display: flex; align-items: flex-end; gap: 10px; height: 31px; }
.wbars { display: inline-flex; align-items: flex-end; gap: 3px; }
.wbars i { width: 5px; background: var(--md-outline-variant); }
.wbars i.on { background: var(--md-primary); box-shadow: 0 0 5px rgba(var(--md-primary-rgb), 0.5); }
.w-db { font-size: 18px; font-weight: 600; color: var(--md-on-surface); line-height: 1; }
.w-db small { font-size: 10px; margin-left: 2px; color: var(--md-on-surface-variant); }
.cloud-row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; padding-top: 2px; }
.cloud-val { display: inline-flex; align-items: center; gap: 6px; font: 600 12px var(--font-body); color: var(--md-on-surface); }
.cloud-url { font-size: 11px; color: var(--md-outline); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; min-width: 0; flex: 1; }

/* eye */
.eye-box { position: relative; flex: 1; min-height: 260px; cursor: pointer; }
.eye-box > .monitor { position: absolute; inset: 0; height: auto; min-height: 0; }
.l-desktop .eye-box { min-height: 320px; }
.l-phone .eye-box { flex: none; min-height: 0; height: min(52vw, 300px); }
.eye-cv { pointer-events: none; }
.eye-box :deep(.eye-canvas) { border-radius: 0; }
.eye-ph { position: absolute; inset: 0; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 8px; color: rgba(255, 255, 255, 0.55); font-size: 13px; }
.rec { color: var(--md-primary); }
.mode-keys { display: flex; flex-wrap: wrap; gap: 6px; }
.mkey {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px;
  border: none;
  border-radius: 3px;
  background: var(--md-surface-container-high);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant), inset 0 -2px 0 var(--ins-key-edge);
  color: var(--md-on-surface-variant);
  font: 600 12px var(--font-body);
  cursor: pointer;
}
.mkey:hover:not(:disabled) { color: var(--md-on-surface); }
.mkey:active:not(:disabled) { transform: translateY(1px); box-shadow: inset 0 0 0 1px var(--md-outline-variant); }
.mkey:disabled { opacity: 0.4; cursor: default; }
.mkey.on { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-primary); }
.mkey.on .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.mkey.exit { color: var(--md-error); }

/* quick */
.qkeys { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 6px; }
.l-phone .qkeys { grid-template-columns: repeat(2, minmax(0, 1fr)); }
.qkey {
  display: flex;
  flex-direction: column;
  gap: 4px;
  padding: 10px;
  min-width: 0;
  border: none;
  border-radius: 3px;
  background: var(--md-surface-container-high);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant), inset 0 -2px 0 var(--ins-key-edge);
  color: var(--md-on-surface);
  text-align: left;
  cursor: pointer;
  transition: box-shadow 0.12s;
}
.qkey:hover { box-shadow: inset 0 0 0 1px var(--md-primary), inset 0 -2px 0 var(--ins-key-edge); }
.qkey:active { transform: translateY(1px); box-shadow: inset 0 0 0 1px var(--md-primary); }
.q-top { display: flex; justify-content: space-between; align-items: center; color: var(--md-primary); }
.q-idx { font: 600 10px var(--ins-mono); color: var(--md-outline); letter-spacing: 0.1em; }
.q-label { font: 600 13.5px var(--font-body); }
.q-hint { font-size: 11.5px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

/* attention */
.count { font-size: 12px; color: var(--md-on-surface-variant); }
.attn { display: flex; flex-direction: column; gap: 1px; background: var(--md-outline-variant); border: 1px solid var(--md-outline-variant); }
.attn-row { display: flex; align-items: center; gap: 10px; padding: 10px 12px; border: none; background: var(--md-surface-container); color: var(--md-on-surface); text-align: left; cursor: pointer; }
.attn-row:hover { background: var(--md-surface-container-high); }
.attn-row.warn :deep(.ui-icon:first-of-type) { color: var(--md-warning); }
.attn-row.err :deep(.ui-icon:first-of-type) { color: var(--md-error); }
.attn-body { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.attn-title { font: 600 13.5px var(--font-body); }
.attn-sub { font-size: 12px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.all-good { display: flex; align-items: center; gap: 10px; font-size: 13px; color: var(--md-on-surface); }

/* plugins */
.plug-stats { display: flex; gap: 24px; }
.plug-stats > div { display: flex; flex-direction: column; gap: 6px; color: var(--md-on-surface); }
.plug-stats .sig { color: var(--md-primary); }
.plug-chips { display: flex; flex-wrap: wrap; gap: 6px; }
.pkey { display: inline-flex; align-items: center; gap: 5px; padding: 5px 9px; border: none; border-radius: 3px; background: transparent; box-shadow: inset 0 0 0 1px var(--md-outline); color: var(--md-on-surface); font: 600 12px var(--font-body); cursor: pointer; }
.pkey:hover { box-shadow: inset 0 0 0 1px var(--md-primary); }

/* ask */
.cli { display: flex; align-items: center; gap: 10px; padding: 0 6px 0 12px; background: var(--md-surface); box-shadow: inset 0 0 0 1px var(--md-outline); border-radius: 3px; }
.cli:focus-within { box-shadow: inset 0 0 0 1px var(--md-primary); }
.prompt { font: 700 16px var(--ins-mono); color: var(--md-primary); animation: blink 1.2s steps(2, jump-none) infinite; }
.cli-in { flex: 1; min-width: 0; background: transparent !important; border: none !important; box-shadow: none !important; outline: none; color: var(--md-on-surface); font: 500 14px var(--ins-mono) !important; padding: 12px 0 !important; }
.cli-send { border: none; border-radius: 2px; padding: 7px 12px; background: var(--md-primary); color: var(--md-on-primary); font: 600 12px var(--ins-mono); cursor: pointer; }
.cli-send:disabled { opacity: 0.35; cursor: default; }
.sugg { display: flex; flex-wrap: wrap; gap: 6px; }
.skey { border: none; border-radius: 2px; padding: 5px 9px; background: var(--md-surface-container-high); box-shadow: inset 0 0 0 1px var(--md-outline-variant); color: var(--md-on-surface-variant); font: 500 12px var(--font-body); cursor: pointer; }
.skey:hover { color: var(--md-on-surface); box-shadow: inset 0 0 0 1px var(--md-outline); }

/* log */
.log { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; }
.log-row { display: grid; grid-template-columns: 64px 8px minmax(0, 1fr); align-items: center; gap: 8px; padding: 7px 0; border-bottom: 1px dashed var(--md-outline-variant); font-size: 12.5px; color: var(--md-on-surface); animation: row-in 0.2s steps(3, jump-none) both; }
.log-row:last-child { border-bottom: none; }
.log-t { font-size: 10.5px; color: var(--md-outline); }
.log-text { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
@keyframes row-in { from { opacity: 0; transform: translateX(-4px); } }
@keyframes blink { 50% { opacity: 0.2; } }
</style>
