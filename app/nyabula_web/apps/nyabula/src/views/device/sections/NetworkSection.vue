<script setup lang="ts">
/* Network: WiFi status from sys.info; scan / switch are contract-reserved. */
import { computed } from 'vue';
import { EmptyState, MdButton, MdCard, MdTextField, Skeleton, UiIcon } from '@nyabula/ui';
import { rssiBars, useSysInfo, wifiOf } from './sysinfo';
import { useDeviceRuntime } from '../../../composables/useDeviceRuntime';

const { task, info } = useSysInfo();
const device = useDeviceRuntime();
const wifi = computed(() => {
  const wireless = device.snapshot?.network.interfaces.find(item => item.ssid);
  return wireless ? { ssid: wireless.ssid ?? null, rssi: null } : wifiOf(info.value);
});
const quality = computed(() => ['无信号', '弱', '一般', '良好', '优秀'][rssiBars(wifi.value.rssi)]);
</script>

<template>
  <div class="stack">
    <MdCard title="WiFi 状态">
      <Skeleton v-if="task.busy.value && !info" :lines="3" />
      <EmptyState v-else-if="!info" tone="error" compact title="读取失败" action-text="重试" @action="task.run()" />
      <div v-else class="wifi">
        <span class="orb"><UiIcon name="wifi" :size="26" /></span>
        <div class="wifi-body">
          <div class="ssid">{{ wifi.ssid ?? 'WiFi 关联状态未提供' }}</div>
          <div class="muted sub">
            <template v-if="wifi.rssi !== null">信号 {{ quality }} · {{ wifi.rssi }} dBm</template>
            <template v-else>无信号数据</template>
          </div>
        </div>
        <span class="tag" :class="wifi.ssid ? 'ok' : 'warn'">{{ wifi.ssid ? '已关联' : '未知' }}</span>
      </div>
    </MdCard>

    <MdCard title="真实网络接口">
      <p v-if="device.error" role="alert">{{ device.error }}</p>
      <p v-for="item in device.snapshot?.network.interfaces" :key="item.index">{{ item.name }} · {{ item.ipv4 ?? '未分配 IPv4' }} · {{ item.up === true ? '已启用' : item.up === false ? '未启用' : '状态未知' }}</p>
      <p v-if="!device.snapshot?.network.interfaces.length" class="muted">{{ device.snapshot?.network.available ? '未发现接口' : '接口枚举未提供' }}</p>
      <MdButton :disabled="!device.available || device.busy" @click="device.refresh()">刷新接口</MdButton>
    </MdCard>

    <MdCard title="扫描网络">
      <div class="row between" style="margin-bottom: 10px">
        <span class="muted" style="font-size: 13px">列出设备附近的 WiFi 热点</span>
        <span class="contract-only">契约预留</span>
      </div>
      <MdButton variant="tonal" disabled><UiIcon name="refresh" :size="16" /> 扫描</MdButton>
    </MdCard>

    <MdCard title="切换网络">
      <div class="row between" style="margin-bottom: 10px">
        <span class="muted" style="font-size: 13px">为设备配置新的 WiFi 凭据</span>
        <span class="contract-only">契约预留</span>
      </div>
      <div class="stack">
        <MdTextField model-value="" label="SSID" placeholder="网络名称" icon="wifi" disabled />
        <MdTextField model-value="" label="密码" type="password" placeholder="••••••••" icon="key" disabled />
        <div class="row" style="justify-content: flex-end"><MdButton disabled>连接</MdButton></div>
      </div>
    </MdCard>
  </div>
</template>

<style scoped>
.wifi { display: flex; align-items: center; gap: 14px; }
.orb {
  width: 48px;
  height: 48px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  background: rgba(var(--md-primary-rgb), 0.14);
  color: var(--md-primary);
  flex: none;
}
.wifi-body { flex: 1; min-width: 0; }
.ssid { font: 600 16px var(--font-title); color: var(--md-on-surface); }
.sub { font-size: 12.5px; margin-top: 2px; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
