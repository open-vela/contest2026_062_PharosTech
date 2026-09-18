<script setup lang="ts">
/* Phone: segmented list; device actions and the claim form open in
 * bottom sheets for one-hand reach. */
import { computed, ref } from 'vue';
import { BottomSheet, MdButton, SegmentedTabs, UiIcon } from '@nyabula/ui';
import AuthForm from './AuthForm.vue';
import AccountOverview from './AccountOverview.vue';
import DeviceList from './DeviceList.vue';
import ClaimForm from './ClaimForm.vue';
import CloudMarketCard from './CloudMarketCard.vue';
import AccountCard from './AccountCard.vue';
import { useAccountPage } from './account.logic';

const page = useAccountPage();
const tab = ref('overview');
const tabs = [
  { id: 'overview', label: '概览' },
  { id: 'devices', label: '设备' },
  { id: 'market', label: '市场' },
];
const claimSheet = ref(false);
const menuId = ref<string | null>(null);
const menuDevice = computed(() => page.account.devices.find((d) => d.deviceId === menuId.value) ?? null);

function act(fn: () => unknown) {
  menuId.value = null;
  fn();
}
</script>

<template>
  <div v-if="!page.account.loggedIn" class="page auth-page">
    <h1 class="page-title">Nyabula Cloud</h1>
    <p class="page-sub">登录后可远程连接已认领的设备。</p>
    <AuthForm :page="page" />
  </div>

  <div v-else class="acct-phone">
    <SegmentedTabs v-model="tab" :items="tabs" stretch />

    <div v-if="tab === 'overview'" class="stack body">
      <AccountOverview :page="page" compact />
      <AccountCard :page="page" />
    </div>

    <div v-else-if="tab === 'devices'" class="stack body">
      <DeviceList :page="page" dense @menu="menuId = $event" @claim="claimSheet = true" />
    </div>

    <div v-else class="stack body">
      <CloudMarketCard />
    </div>

    <div v-if="tab === 'devices'" class="fab-wrap">
      <MdButton class="fab" @click="claimSheet = true"><UiIcon name="add" :size="20" /> 认领设备</MdButton>
    </div>

    <BottomSheet :open="claimSheet" title="认领设备" @close="claimSheet = false">
      <ClaimForm :page="page" @done="claimSheet = false" />
    </BottomSheet>

    <BottomSheet :open="!!menuDevice" :title="menuDevice?.name || menuDevice?.deviceId" @close="menuId = null">
      <div v-if="menuDevice" class="stack">
        <p class="muted mono" style="font-size: 12.5px; margin: 0">{{ menuDevice.deviceId }} · {{ menuDevice.online ? '在线' : '离线' }}</p>
        <button class="list-tile" :disabled="!menuDevice.online" @click="act(() => page.openRemote(menuDevice!.deviceId))">
          <div class="tile-icon"><UiIcon name="link" :size="20" /></div>
          <div class="tile-body"><div class="tile-title">远程连接</div><div class="tile-sub">{{ menuDevice.online ? '通过 Cloud 中继接管眼睛' : '设备离线，暂不可连' }}</div></div>
        </button>
        <button class="list-tile" @click="act(() => page.openStats(menuDevice!.deviceId))">
          <div class="tile-icon"><UiIcon name="dashboard" :size="20" /></div>
          <div class="tile-body"><div class="tile-title">统计</div><div class="tile-sub">每日在线时长与帧数</div></div>
        </button>
        <button class="list-tile" @click="act(() => page.unclaim(menuDevice!.deviceId, menuDevice!.name))">
          <div class="tile-icon danger"><UiIcon name="link_off" :size="20" /></div>
          <div class="tile-body"><div class="tile-title">解绑</div><div class="tile-sub">从账号移除此设备</div></div>
        </button>
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.auth-page { padding: 24px 16px 40px; }
.acct-phone { display: flex; flex-direction: column; gap: 12px; padding: 12px 14px calc(var(--shell-bottom) + var(--safe-b) + 84px); }
.body { gap: 12px; }
.fab-wrap {
  position: fixed;
  right: 16px;
  bottom: calc(var(--shell-bottom) + var(--safe-b) + 16px);
  z-index: 5;
}
.fab { box-shadow: var(--md-elev-2, var(--md-elev-1)); }
.list-tile:disabled { opacity: 0.5; cursor: not-allowed; }
.tile-icon.danger { background: color-mix(in srgb, var(--md-error) 18%, transparent); color: var(--md-error); }
</style>
