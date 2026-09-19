<script setup lang="ts">
/* Device build only: stands in for the address form. Runs the access boot
 * decision for the device that served the page (sys.auth.state -> hello with
 * the pair or the session token) and leaves for /setup-password, /login or
 * the app. The route guard then seals /provision while the device is offline. */
import { watch } from 'vue';
import { useRouter } from 'vue-router';
import { EmptyState, MdCard, Skeleton } from '@nyabula/ui';
import GateFrame from '../../components/gate/GateFrame.vue';
import { useDeviceAccessStore } from '../../stores/deviceAccess';

const router = useRouter();
const access = useDeviceAccessStore();

/* One trigger per change: boot() while undecided, leave once it has decided. */
watch(
  () => [access.phase, access.unreachable] as const,
  ([phase, unreachable]) => {
    if (phase === 'boot') {
      if (!unreachable) void access.boot();
    } else if (phase === 'login') void router.replace({ name: 'login' });
    else if (phase === 'setup') void router.replace({ name: 'setup-password' });
    else void router.replace(access.nextLocation());
  },
  { immediate: true },
);
</script>

<template>
  <GateFrame :title="access.unreachable ? '连接不上设备' : '正在连接设备'">
    <MdCard>
      <EmptyState v-if="access.unreachable" tone="error" compact title="没有连上" hint="请确认手机或电脑和设备在同一个网络里，然后重试。" action-text="重试" @action="access.boot()" />
      <Skeleton v-else :lines="3" />
    </MdCard>
  </GateFrame>
</template>
