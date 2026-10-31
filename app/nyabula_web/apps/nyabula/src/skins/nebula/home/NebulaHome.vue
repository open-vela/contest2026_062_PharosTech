<script setup lang="ts">
import PluginIcon from '../../../components/PluginIcon.vue';
import { roleLabel } from '@nyabula/ui';
/* Nebula home (design-lab A): the live eyes are the hero, floating on the
 * aurora; vitals on a glass strip; below a band of attention + ask, quick
 * actions; plugins and widgets close the page.
 * Parity with views/home (same useHomePage): device name / state / role /
 * Core / cloud tags, battery, WiFi, uptime, cloud relay, pairing overlay,
 * quick expressions + exit scene, quick actions, attention, plugins
 * summary, Nyabot quick ask, widget grid. */
import { computed, inject, ref } from 'vue';
import { useRouter } from 'vue-router';
import { PairOverlay, UiIcon } from '@nyabula/ui';
import WidgetGrid from '../../../components/WidgetGrid.vue';
import type { useFormFactor } from '../../../composables/useFormFactor';
import { useHomePage } from '../../../views/home/home.logic';
import EyeMirrorCard from '../../../views/eye/EyeMirrorCard.vue';
import NebulaEyeCanvas from '../parts/NebulaEyeCanvas.vue';
import NbRing from '../parts/NbRing.vue';

const page = useHomePage();
const { session, eye, plugins, go } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const form = computed(() => (ff.formFactor.value === 'tablet' && ff.orientation.value === 'portrait' ? 'tablet-portrait' : ff.formFactor.value));
const router = useRouter();

const greeting = computed(() => {
  const h = new Date().getHours();
  return h < 6 ? '夜深了' : h < 11 ? '早上好' : h < 14 ? '中午好' : h < 18 ? '下午好' : '晚上好';
});
const headline = computed(() => {
  if (session.state === 'pairing-required') return '它在等你配对';
  if (!session.connected) return session.state === 'connecting' || session.state === 'reconnecting' || session.state === 'authenticating' ? '正在唤醒它' : '它睡着了';
  if (!eye.ready) return '它醒着';
  return eye.activeMode === 'sleep' ? '它正睡着' : '你好，主人';
});

/* Pairing from the hero (same flow as EyePreviewCard). */
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
const showPair = computed(() => session.state === 'pairing-required' || (pairing.value && session.state === 'authenticating'));

/* Cloud relay line (from cloud.status). */
const cloudLine = computed(() => {
  const c = page.cloud.value;
  if (!session.connected || !c) return { label: '—', sub: '云中继', tone: '' };
  if (c.state === 'unsupported') return { label: '暂不支持', sub: '云中继', tone: '' };
  if (c.state === 'connecting') return { label: '连接中', sub: '云中继', tone: 'warn' };
  if (!c.enabled) return { label: '未启用', sub: '云中继', tone: '' };
  return c.state === 'online' ? { label: '已连通', sub: '云中继', tone: 'ok' } : { label: '未连通', sub: '云中继', tone: 'warn' };
});
const batteryIcon = computed(() => (page.battery.value.external ? 'bolt' : page.battery.value.charging ? 'battery_charging' : page.battery.value.level !== null && page.battery.value.level <= 20 ? 'battery_low' : 'battery'));

/* Nyabot quick ask (same routing as AgentQuickAsk). */
const ask = ref('');
const SUGGESTIONS = ['今天天气怎么样？', '设个 10 分钟倒计时', '播放点音乐', '你现在心情如何？'];
function send(t = ask.value) {
  const q = t.trim();
  if (!q) return;
  void router.push({ name: 'agent', params: { key: session.deviceKey ?? '' }, query: { q } });
}
</script>

<template>
  <div class="nb-home" :class="form">
    <!-- Hero: copy, floating eyes, quick expressions -->
    <section class="hero">
      <div class="copy nb-in">
        <p class="nb-kicker">{{ greeting }} · {{ page.deviceName.value }}</p>
        <h1 class="nb-title nb-grad headline">{{ headline }}</h1>
        <p class="sub">
          <template v-if="eye.ready">当前状态 <b>{{ page.modeLabel.value }}</b><template v-if="page.sceneLabel.value"> · 场景 <b>{{ page.sceneLabel.value }}</b></template></template>
          <template v-else>{{ page.stateLabel.value }}</template>
        </p>
        <div class="tags">
          <span class="nb-tag" :class="page.stateTone.value"><span class="nb-dot" />{{ page.stateLabel.value }}</span>
          <span v-if="session.role" class="nb-tag">{{ roleLabel(session.role) }}</span>
          <span v-if="page.coreVersion.value" class="nb-tag">Core {{ page.coreVersion.value }}</span>
          <span v-if="session.transport === 'cloud'" class="nb-tag info"><UiIcon name="cloud" :size="12" />云中继</span>
        </div>
      </div>
      <!-- data-eyes-*: the Nebula boot lands its eyes here and settles into this expression. -->
      <div class="eyes" :class="{ live: eye.ready }" data-eyes-anchor :data-eyes-mode="eye.ready ? eye.activeMode : 'sleep'" @click="session.connected && go('eye')">
        <div class="halo" />
        <EyeMirrorCard v-if="eye.ready && eye.nativeQr" embedded auto-start />
        <NebulaEyeCanvas v-else-if="eye.ready" :eye-state="eye.lastState" :clock-offset-ms="eye.nativeCore ? 0 : session.clockOffsetMs()" :interactive="false" />
        <NebulaEyeCanvas v-else mode="sleep" :interactive="false" />
        <PairOverlay v-if="showPair" class="pair" :busy="pairing" :error="pairError" @submit="doPair" @click.stop />
      </div>
      <div class="moods nb-in" style="--d: 200ms">
        <button
          v-for="m in page.quickModes"
          :key="m.id"
          class="mood"
          :class="{ on: eye.activeMode === m.id && !eye.activeScene }"
          :disabled="!session.canControl || !eye.ready"
          @click="eye.setMode(m.id)"
        >{{ m.label }}</button>
        <button v-if="eye.activeScene" class="mood" @click="eye.setScene(null)"><UiIcon name="close" :size="14" /> 退出场景</button>
        <button class="mood more" :disabled="!session.connected" @click="go('eye')">完整控制 <UiIcon name="arrow_forward" :size="14" /></button>
      </div>
    </section>

    <!-- Vitals -->
    <section class="vitals nb-glass nb-in" style="--d: 120ms">
      <div class="vital">
        <NbRing :value="page.battery.value.level !== null ? page.battery.value.level / 100 : null" :color="page.battery.value.level !== null && page.battery.value.level <= 20 && !page.battery.value.charging ? 'var(--md-error)' : 'var(--nb-acc)'">
          <UiIcon :name="batteryIcon" :size="18" />
        </NbRing>
        <div>
          <div class="v-val">{{ page.battery.value.external ? '已连接电源' : page.battery.value.level ?? '—' }}<small v-if="page.battery.value.level !== null">%</small></div>
          <div class="v-key">{{ page.battery.value.external ? '供电' : page.battery.value.charging ? '充电中' : '电量' }}</div>
        </div>
      </div>
      <div class="vital">
        <span class="wifi" :title="page.wifi.value.rssi !== null ? page.wifi.value.rssi + ' dBm' : ''">
          <i v-for="i in 4" :key="i" :class="{ on: i <= page.bars.value }" :style="{ height: 5 + i * 4 + 'px', animationDelay: i * 80 + 'ms' }" />
        </span>
        <div class="v-text">
          <div class="v-val sm">{{ page.wifi.value.ssid ?? '—' }}</div>
          <div class="v-key">WiFi{{ page.wifi.value.rssi !== null ? ` · ${page.wifi.value.rssi} dBm` : '' }}</div>
        </div>
      </div>
      <div class="vital">
        <span class="v-ico"><UiIcon name="schedule" :size="20" /></span>
        <div class="v-text"><div class="v-val sm">{{ page.uptime.value }}</div><div class="v-key">持续陪伴</div></div>
      </div>
      <button class="vital link" @click="go('device', { section: 'cloud' })">
        <span class="v-ico" :class="cloudLine.tone"><UiIcon :name="cloudLine.tone === 'warn' ? 'cloud_off' : 'cloud'" :size="20" /></span>
        <div class="v-text"><div class="v-val sm">{{ cloudLine.label }}</div><div class="v-key">{{ cloudLine.sub }}</div></div>
      </button>
      <button class="vital link" @click="go('plugins')">
        <NbRing :value="page.pluginCount.value ? page.runningCount.value / page.pluginCount.value : null" color="var(--nb-pink)"><UiIcon name="extension" :size="18" /></NbRing>
        <div><div class="v-val">{{ page.runningCount.value }}<small>/{{ page.pluginCount.value }}</small></div><div class="v-key">插件运行中</div></div>
      </button>
    </section>

    <!-- Band -->
    <section class="band">
      <article class="card nb-glass nb-in" style="--d: 260ms">
        <p class="nb-kicker">需要留意</p>
        <div v-if="page.attention.value.length" class="att">
          <button v-for="a in page.attention.value" :key="a.id" class="att-row" :class="a.tone" @click="a.run()">
            <span class="att-ico"><UiIcon :name="a.icon" :size="18" /></span>
            <span class="att-text" :title="`${a.title}\n${a.sub}`"><b>{{ a.title }}</b><small>{{ a.sub }}</small></span>
            <UiIcon name="chevron_right" :size="18" />
          </button>
        </div>
        <p v-else class="all-good"><UiIcon name="check_circle" :size="18" /> 一切正常，没有待处理事项</p>
        <form class="ask" @submit.prevent="send()">
          <UiIcon name="auto_awesome" :size="18" />
          <input v-model="ask" placeholder="问问 Nyabot…" />
          <button type="submit" :disabled="!ask.trim()" title="发送"><UiIcon name="send" :size="16" /></button>
        </form>
        <div class="sugg">
          <button v-for="s in SUGGESTIONS" :key="s" @click="send(s)">{{ s }}</button>
        </div>
      </article>

      <article class="card nb-glass nb-in" style="--d: 320ms">
        <p class="nb-kicker">快捷操作</p>
        <div class="qa">
          <button v-for="(a, i) in page.quickActions.value" :key="a.id" class="qa-item" :style="{ animationDelay: 380 + i * 50 + 'ms' }" @click="a.run()">
            <span class="qa-ico"><UiIcon :name="a.icon" :size="20" /></span>
            <span class="qa-label">{{ a.label }}</span>
            <span v-if="a.hint" class="qa-hint">{{ a.hint }}</span>
          </button>
        </div>
      </article>


    </section>

    <!-- Plugins + widgets -->
    <section class="tail">
      <article class="card nb-glass nb-in plugins" style="--d: 420ms">
        <div class="p-head">
          <p class="nb-kicker">插件</p>
          <button class="p-all" @click="go('plugins')">全部 <UiIcon name="chevron_right" :size="16" /></button>
        </div>
        <div v-if="plugins.list.length" class="p-chips">
          <button v-for="p in plugins.list.slice(0, 6)" :key="p.id" class="p-chip" @click="go('plugin', { id: p.id })">
            <PluginIcon :plugin="p" :size="16" />{{ p.name }}
          </button>
        </div>
        <p v-else class="feed-empty">尚未安装插件。</p>
      </article>
      <WidgetGrid class="widgets nb-in" style="--d: 460ms" :form-factor="ff.formFactor.value" />
    </section>
  </div>
</template>

<style scoped>
.nb-home { display: flex; flex-direction: column; gap: 22px; padding: 0 clamp(16px, 3vw, 36px) 24px; max-width: 1320px; margin: 0 auto; width: 100%; }

/* hero */
.hero { position: relative; display: grid; grid-template-rows: auto minmax(260px, 44vh) auto; justify-items: center; text-align: center; padding-top: 1vh; }
.headline { font-size: clamp(34px, 5vw, 64px); margin: 8px 0 6px; line-height: 1.08; }
.sub { color: var(--nb-sub); margin: 0; }
.sub b { color: var(--nb-acc); font-weight: 600; }
.tags { display: flex; flex-wrap: wrap; justify-content: center; gap: 6px; margin-top: 12px; }
/* No entrance of their own: on first load the boot's eyes land here. */
.eyes { position: relative; width: min(760px, 100%); height: 100%; }
.eyes.live { cursor: pointer; }
.eyes :deep(.nb-eye-canvas) { position: absolute; inset: 0; }
.halo { position: absolute; inset: 12% 18%; border-radius: 50%; background: radial-gradient(closest-side, rgba(var(--nb-acc-rgb), 0.16), transparent); filter: blur(30px); animation: nb-breathe 6s ease-in-out infinite; pointer-events: none; }
@keyframes nb-breathe { 50% { transform: scale(1.08); opacity: 0.7; } }
.pair { z-index: 3; }
.moods { display: flex; flex-wrap: wrap; justify-content: center; gap: 8px; margin-top: 4px; }
.mood {
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-sub);
  padding: 7px 14px;
  border-radius: 999px;
  font: 600 13px var(--font-body);
  cursor: pointer;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  transition: color 0.3s var(--nb-ease), background 0.3s var(--nb-ease), border-color 0.3s, box-shadow 0.3s, transform 0.35s var(--nb-spring);
}
.mood:hover:not(:disabled) { color: var(--nb-fg); border-color: var(--nb-hi); transform: translateY(-2px); }
.mood:active:not(:disabled) { transform: scale(0.94); }
.mood:disabled { opacity: 0.4; cursor: default; }
.mood.on { color: var(--nb-on-acc); background: var(--nb-acc); border-color: transparent; box-shadow: 0 6px 22px -6px rgba(var(--nb-acc-rgb), 0.8); }
.mood.more { border-style: dashed; }

/* vitals */
.vitals { display: grid; grid-template-columns: repeat(5, auto); justify-content: space-between; align-items: center; gap: 12px 28px; padding: 18px 26px; }
.vital { display: flex; align-items: center; gap: 12px; min-width: 0; border: none; background: none; color: inherit; font: inherit; text-align: left; padding: 0; }
.vital.link { cursor: pointer; border-radius: 16px; transition: transform 0.35s var(--nb-spring); }
.vital.link:hover { transform: translateY(-2px); }
.v-text { min-width: 0; }
.v-val { font: 400 22px/1.1 var(--font-title); color: var(--nb-fg); }
.v-val small { font: 600 13px var(--font-body); color: var(--nb-sub); margin-left: 1px; }
.v-val.sm { font: 600 15px var(--font-body); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 180px; }
.v-key { font-size: 12px; color: var(--nb-sub); margin-top: 2px; }
.v-ico { width: 50px; height: 50px; border-radius: 50%; display: grid; place-items: center; background: var(--nb-soft-2); color: var(--nb-sub); flex: none; }
.v-ico.ok { color: var(--nb-acc); background: rgba(var(--nb-acc-rgb), 0.12); }
.v-ico.warn { color: var(--nb-amber); }
.wifi { width: 50px; height: 50px; display: flex; align-items: flex-end; justify-content: center; gap: 3px; padding-bottom: 14px; flex: none; }
.wifi i { width: 4px; border-radius: 2px; background: var(--nb-soft-2); transform-origin: bottom; animation: nb-grow 0.7s var(--nb-spring) both; }
.wifi i.on { background: var(--nb-acc); box-shadow: 0 0 8px rgba(var(--nb-acc-rgb), 0.6); }
@keyframes nb-grow { from { transform: scaleY(0); } }

/* band */
.band { display: grid; grid-template-columns: minmax(0, 1.1fr) minmax(0, 1fr); gap: 18px; }
.card { padding: 20px 22px; display: flex; flex-direction: column; gap: 14px; min-width: 0; transition: transform 0.5s var(--nb-ease), border-color 0.3s; }
.card:hover { transform: translateY(-3px); border-color: var(--nb-line-strong); }
.att { display: grid; grid-template-columns: minmax(0, 1fr); gap: 8px; }
.att-row {
  display: flex;
  min-width: 0;
  width: 100%;
  box-sizing: border-box;
  align-items: center;
  gap: 12px;
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-sub);
  border-radius: 16px;
  padding: 10px 12px;
  cursor: pointer;
  text-align: left;
  font: inherit;
  transition: background 0.3s var(--nb-ease), transform 0.3s var(--nb-ease);
}
.att-row:hover { background: var(--nb-soft-2); transform: translateX(3px); }
.att-ico { width: 36px; height: 36px; border-radius: 12px; display: grid; place-items: center; background: rgba(var(--nb-violet-rgb), 0.14); color: var(--nb-violet); flex: none; }
.att-row.warn .att-ico { background: color-mix(in srgb, var(--nb-amber) 14%, transparent); color: var(--nb-amber); }
.att-row.err .att-ico { background: color-mix(in srgb, var(--md-error) 14%, transparent); color: var(--md-error); }
.att-text { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.att-text b { color: var(--nb-fg); font-weight: 600; font-size: 13.5px; overflow-wrap: anywhere; }
.att-text small { font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.all-good { display: flex; align-items: center; gap: 8px; margin: 0; color: var(--nb-acc); font-size: 13.5px; }
.ask {
  margin-top: auto;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 6px 6px 6px 14px;
  border-radius: 999px;
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-violet);
  transition: border-color 0.2s, box-shadow 0.3s;
}
.ask:focus-within { border-color: rgba(var(--nb-violet-rgb), 0.6); box-shadow: 0 0 0 4px rgba(var(--nb-violet-rgb), 0.12); }
.ask input { flex: 1; min-width: 0; border: none; background: transparent; outline: none; color: var(--nb-fg); font: 600 14px var(--font-body); padding: 0; }
.ask input::placeholder { color: var(--nb-dim); font-weight: 500; }
.ask button { width: 34px; height: 34px; border-radius: 50%; border: none; background: var(--nb-violet); color: #fff; display: grid; place-items: center; cursor: pointer; transition: transform 0.35s var(--nb-spring), opacity 0.2s; }
.ask button:disabled { opacity: 0.3; cursor: default; }
.ask button:not(:disabled):hover { transform: scale(1.1) rotate(-12deg); }
.sugg { display: flex; flex-wrap: wrap; gap: 6px; margin-top: -6px; }
.sugg button { border: none; background: var(--nb-soft-2); color: var(--nb-sub); font: 500 12px var(--font-body); padding: 5px 10px; border-radius: 999px; cursor: pointer; transition: color 0.2s, transform 0.3s var(--nb-spring); }
.sugg button:hover { color: var(--nb-fg); transform: translateY(-1px); }

.qa { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
.qa-item {
  display: grid;
  grid-template-columns: auto 1fr;
  grid-template-rows: auto auto;
  column-gap: 10px;
  align-items: center;
  padding: 12px;
  border-radius: 18px;
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-fg);
  cursor: pointer;
  text-align: left;
  font: inherit;
  animation: nb-rise 0.6s var(--nb-ease) both;
  transition: background 0.25s, border-color 0.25s, transform 0.35s var(--nb-spring);
}
.qa-item:hover { background: var(--nb-soft-2); border-color: var(--nb-line-strong); transform: translateY(-2px); }
.qa-item:active { transform: scale(0.96); }
.qa-ico { grid-row: span 2; width: 38px; height: 38px; border-radius: 12px; display: grid; place-items: center; background: rgba(var(--nb-acc-rgb), 0.13); color: var(--nb-acc); }
.qa-label { font: 600 13.5px var(--font-body); }
.qa-hint { font-size: 11.5px; color: var(--nb-sub); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }

.feed-empty { margin: 0; color: var(--nb-sub); font-size: 13px; line-height: 1.6; }

/* tail */
.tail { display: grid; grid-template-columns: minmax(280px, 1fr) 2fr; gap: 18px; align-items: start; }
.tail .widgets { min-width: 0; }
.tail .widgets :deep(.section-title) { margin-top: 0; }
.p-head { display: flex; align-items: center; justify-content: space-between; }
.p-all { border: none; background: none; color: var(--nb-acc); font: 600 13px var(--font-body); cursor: pointer; display: inline-flex; align-items: center; padding: 0; }
.p-chips { display: flex; flex-wrap: wrap; gap: 8px; }
.p-chip { display: inline-flex; align-items: center; gap: 6px; padding: 7px 12px; border-radius: 999px; border: 1px solid var(--nb-line); background: var(--nb-soft); color: var(--nb-fg); font: 600 12.5px var(--font-body); cursor: pointer; transition: background 0.2s, transform 0.35s var(--nb-spring); }
.p-chip:hover { background: var(--nb-soft-2); transform: translateY(-1px); }

/* form factors */
.tablet .vitals, .tablet-portrait .vitals { grid-template-columns: repeat(3, auto); }
.tablet .band { grid-template-columns: 1fr 1fr; }
.tablet-portrait .band, .tablet-portrait .tail { grid-template-columns: 1fr; }
.tablet-portrait .hero { grid-template-rows: auto 34vh auto; }
.phone { gap: 16px; padding: 0 14px 16px; }
.phone .hero { grid-template-rows: auto 30vh auto; }
.phone .headline { font-size: 34px; }
.phone .vitals { grid-template-columns: 1fr 1fr; padding: 16px; gap: 16px; }
.phone .band, .phone .tail { grid-template-columns: 1fr; }
.phone .moods { flex-wrap: nowrap; justify-content: flex-start; overflow-x: auto; width: 100%; scrollbar-width: none; padding-bottom: 2px; }
.phone .moods::-webkit-scrollbar { display: none; }
.phone .mood { flex: none; padding: 6px 12px; font-size: 12.5px; }
@media (max-width: 1180px) {
  .desktop .vitals { grid-template-columns: repeat(3, auto); }
  .desktop .band { grid-template-columns: 1fr 1fr; }
}
</style>
