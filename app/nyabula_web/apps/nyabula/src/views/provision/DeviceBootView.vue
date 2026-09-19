<script setup lang="ts">
/* Device build only: stands in for the address form. Connects to the device
 * that served the page and decides between WiFi provisioning and home. */
import { watch } from 'vue';
import { useRouter } from 'vue-router';
import { EmptyState, Skeleton } from '@nyabula/ui';
import { SELF_KEY } from '../../stores/session';
import { useDeviceLink } from '../../composables/useDeviceLink';
import { parseNetworkStatus } from '../../lib/wifi';

const router = useRouter();
const link = useDeviceLink(SELF_KEY);

watch(
  () => link.gate.value,
  async (gate) => {
    if (gate === 'pairing') {
      // No token on this origin yet: the workspace owns the pairing overlay.
      await router.replace({ name: 'home', params: { key: SELF_KEY } });
      return;
    }
    if (gate !== 'ready') return;
    let online = true;
    try {
      online = parseNetworkStatus(await link.session.request('network.status')).state === 'sta_online';
    } catch {
      /* Status unavailable: home still works, WiFi can be changed from there. */
    }
    await router.replace(online ? { name: 'home', params: { key: SELF_KEY } } : { name: 'provision' });
  },
  { immediate: true },
);
</script>

<template>
  <div class="page narrow boot">
    <EmptyState
      v-if="link.gate.value === 'auth'"
      icon="qr_code"
      title="请重新扫描设备上的二维码"
      hint="访问凭证已经失效。请用手机相机重新扫描设备眼睛屏幕上的二维码，再从扫出来的链接进入。"
      action-text="再试一次"
      @action="link.retry()"
    />
    <EmptyState v-else-if="link.gate.value === 'unreachable'" tone="error" title="连接不上设备" hint="请确认手机或电脑和设备在同一个网络里，然后重试。" action-text="重试" @action="link.retry()" />
    <Skeleton v-else :lines="3" />
  </div>
</template>

<style scoped>
.boot { padding-top: 48px; }
</style>
