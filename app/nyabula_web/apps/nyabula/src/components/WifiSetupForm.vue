<script setup lang="ts">
/* Scan + pick + credentials form, shared by the provisioning page and the
 * device network section. All state lives in useWifiSetup(). */
import { ref } from 'vue';
import { EmptyState, MdButton, MdSwitch, MdTextField, Skeleton, UiIcon } from '@nyabula/ui';
import type { WifiSetup } from '../composables/useWifiSetup';
import { bandLabel } from '../lib/wifi';
import { rssiBars } from '../views/device/sections/sysinfo';

defineProps<{ setup: WifiSetup; submitText?: string }>();
const showPsk = ref(false);
</script>

<template>
  <div class="stack wifi-form">
    <div class="row between">
      <span class="muted scan-hint">{{ setup.scanning.value ? '正在扫描，大约需要几秒…' : setup.scanned.value ? `附近有 ${setup.networks.value.length} 个网络` : '扫描设备附近的 WiFi' }}</span>
      <MdButton variant="tonal" :disabled="setup.scanning.value || !setup.session.connected" @click="setup.scan()">
        <UiIcon :name="setup.scanning.value ? 'sync' : 'refresh'" :size="16" :class="{ spin: setup.scanning.value }" /> {{ setup.scanned.value ? '重新扫描' : '扫描' }}
      </MdButton>
    </div>

    <Skeleton v-if="setup.scanning.value && !setup.networks.value.length" :lines="3" />
    <EmptyState v-else-if="setup.scanError.value" tone="error" compact title="扫描失败" :hint="setup.scanError.value" action-text="重试" @action="setup.scan()" />
    <EmptyState v-else-if="setup.scanned.value && !setup.networks.value.length" icon="wifi" compact title="没有发现网络" hint="可以靠近路由器后重新扫描，或直接输入 WiFi 名称" />
    <ul v-else-if="setup.networks.value.length" class="nets">
      <li v-for="net in setup.networks.value" :key="net.ssid">
        <button type="button" class="list-tile net" :class="{ picked: net.ssid === setup.ssid.value }" @click="setup.pick(net)">
          <span class="bars" :data-bars="rssiBars(net.rssi)" aria-hidden="true"><i /><i /><i /><i /></span>
          <span class="tile-body">
            <span class="tile-title net-name">{{ net.ssid }}</span>
            <span class="tile-sub">{{ [net.secure === false ? '开放网络' : net.secure ? '需要密码' : '', bandLabel(net.freq), net.rssi !== null ? `${net.rssi} dBm` : ''].filter(Boolean).join(' · ') }}</span>
          </span>
          <span class="tile-trail">
            <UiIcon v-if="net.secure !== false" name="lock" :size="16" />
            <UiIcon v-if="net.ssid === setup.ssid.value" name="check_circle" :size="18" class="picked-mark" />
          </span>
        </button>
      </li>
    </ul>

    <MdTextField v-model="setup.ssid.value" label="WiFi 名称" placeholder="从上方选择，或手动输入" icon="wifi" autocomplete="off" />
    <template v-if="setup.secure.value !== false">
      <MdTextField
        v-model="setup.psk.value"
        label="WiFi 密码"
        :type="showPsk ? 'text' : 'password'"
        :placeholder="setup.secure.value ? '8–63 个字符' : '开放网络可留空'"
        icon="key"
        autocomplete="off"
        @enter="setup.submit()"
      />
      <MdSwitch v-model="showPsk" label="显示密码" />
    </template>
    <p v-else class="muted open-note">这是开放网络，不需要密码。</p>

    <p v-if="setup.formError.value" class="form-error" role="alert">{{ setup.formError.value }}</p>
    <p v-if="setup.session.connected && !setup.session.isOwner" class="muted open-note">只有设备主人可以修改 WiFi。</p>
    <MdButton class="submit" :disabled="!setup.canSubmit.value" @click="setup.submit()">{{ setup.submitting.value ? '正在发送…' : (submitText ?? '连接') }}</MdButton>
  </div>
</template>

<style scoped>
.wifi-form { gap: 12px; }
.scan-hint { font-size: 13px; min-width: 0; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
.nets { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 6px; max-height: 320px; overflow-y: auto; }
.net { background: var(--md-surface-container-high); }
.net.picked { box-shadow: inset 0 0 0 2px var(--md-primary); }
.net .tile-title, .net .tile-sub { display: block; }
.net-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.picked-mark { color: var(--md-primary); }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; width: 22px; height: 18px; flex: none; }
.bars i { flex: 1; border-radius: 1px; background: var(--md-outline-variant); }
.bars i:nth-child(1) { height: 25%; }
.bars i:nth-child(2) { height: 50%; }
.bars i:nth-child(3) { height: 75%; }
.bars i:nth-child(4) { height: 100%; }
.bars[data-bars='1'] i:nth-child(-n + 1),
.bars[data-bars='2'] i:nth-child(-n + 2),
.bars[data-bars='3'] i:nth-child(-n + 3),
.bars[data-bars='4'] i:nth-child(-n + 4) { background: var(--md-primary); }
.open-note { font-size: 13px; margin: 0; }
.form-error { margin: 0; font-size: 13px; color: var(--md-error); }
.submit { width: 100%; padding: 13px 22px; }
.spin { animation: wifi-spin 1.2s linear infinite; }
@keyframes wifi-spin { to { transform: rotate(360deg); } }
</style>
