<script setup lang="ts">
/* Device at a glance: name, state, role, core version, battery, WiFi,
 * uptime. Compact mode stacks facts for narrow columns. */
import { computed } from 'vue';
import { MdCard, NyabulaLogo, Skeleton, UiIcon } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage>; compact?: boolean }>();
const { session, deviceName, coreVersion, stateLabel, stateTone, battery, wifi, bars, uptime, info, infoTask } = props.page;

/* Fact tile texts (also the `title` tooltips: tiles ellipsize when narrow). */
const batteryIcon = computed(() => (battery.value.external ? 'bolt' : battery.value.charging ? 'battery_charging' : battery.value.level !== null && battery.value.level <= 20 ? 'battery_low' : 'battery'));
const batteryVal = computed(() => (battery.value.external ? '已连接电源' : battery.value.level !== null ? battery.value.level + '%' : '—'));
const batteryKey = computed(() => (battery.value.external ? '供电' : battery.value.charging ? '充电中' : '电量'));
const wifiVal = computed(() => wifi.value.ssid ?? '—');
const wifiKey = computed(() => `WiFi${wifi.value.rssi !== null ? ` · ${wifi.value.rssi} dBm` : ''}`);
</script>

<template>
  <MdCard class="hero" :class="{ compact }">
    <div class="hero-top">
      <NyabulaLogo :size="compact ? 48 : 64" />
      <div class="hero-id">
        <h2 class="hero-name" :title="deviceName">{{ deviceName }}</h2>
        <div class="row wrap" style="gap: 6px">
          <span class="tag" :class="stateTone"><span class="dot" />{{ stateLabel }}</span>
          <span v-if="session.role" class="tag">{{ session.role }}</span>
          <span v-if="coreVersion" class="tag mono">Core {{ coreVersion }}</span>
          <span v-if="session.transport === 'cloud'" class="tag info"><UiIcon name="cloud" :size="13" /> 云中继</span>
        </div>
      </div>
    </div>
    <div class="facts">
      <div class="fact">
        <UiIcon :name="batteryIcon" :size="20" />
        <div class="fact-body">
          <div class="fact-val" :title="batteryVal">{{ batteryVal }}</div>
          <div class="fact-key" :title="batteryKey">{{ batteryKey }}</div>
        </div>
      </div>
      <div class="fact">
        <span class="bars" :title="wifi.rssi !== null ? wifi.rssi + ' dBm' : ''">
          <i v-for="i in 4" :key="i" :class="{ on: i <= bars }" :style="{ height: 4 + i * 3 + 'px' }" />
        </span>
        <div class="fact-body">
          <div class="fact-val" :title="wifiVal">{{ wifiVal }}</div>
          <div class="fact-key" :title="wifiKey">{{ wifiKey }}</div>
        </div>
      </div>
      <div class="fact">
        <UiIcon name="schedule" :size="20" />
        <div class="fact-body">
          <div class="fact-val" :title="uptime">{{ uptime }}</div>
          <div class="fact-key">运行时长</div>
        </div>
      </div>
    </div>
    <Skeleton v-if="infoTask.busy.value && !info" :lines="2" />
  </MdCard>
</template>

<style scoped>
.hero { display: flex; flex-direction: column; gap: 18px; }
.hero-top { display: flex; align-items: center; gap: 16px; }
.hero-id { min-width: 0; display: flex; flex-direction: column; gap: 8px; }
.hero-name { font: 600 22px var(--font-title); color: var(--md-on-surface); margin: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.compact .hero-name { font-size: 18px; }
.dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; display: inline-block; margin-right: 4px; }
/* minmax(0, 1fr): a long SSID must not widen its column past the card. */
.facts { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }
.compact .facts { grid-template-columns: minmax(0, 1fr); gap: 10px; }
.fact {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 12px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  color: var(--md-on-surface-variant);
  min-width: 0;
  overflow: hidden;
}
.fact > .ui-icon { flex: none; }
/* Shrinkable text column: both lines stay on one line and ellipsize. */
.fact-body { flex: 1; min-width: 0; }
.fact-val { font: 600 15px var(--font-body); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.fact-key { font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; width: 20px; height: 16px; flex: none; }
.bars i { width: 3px; border-radius: 1px; background: var(--md-outline-variant); }
.bars i.on { background: var(--md-primary); }
</style>
