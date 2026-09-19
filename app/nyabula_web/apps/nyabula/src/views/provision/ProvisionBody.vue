<script setup lang="ts">
/* Provisioning content shared by the phone and desktop variants; the
 * variants only differ in the frame around it. */
import { computed } from 'vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import WifiSetupForm from '../../components/WifiSetupForm.vue';
import WifiJoinNotice from '../../components/WifiJoinNotice.vue';
import type { ProvisionPage } from './provision.logic';

const props = defineProps<{ page: ProvisionPage }>();

const gate = computed(() => props.page.link.gate.value);
const status = computed(() => props.page.wifi.status.value);
const authHint = computed(() => {
  if (!props.page.deviceBuild) return '当前会话没有修改 WiFi 的权限，请回到连接页重新认证。';
  return gate.value === 'auth'
    ? '这个链接里的访问凭证已经失效。请用手机相机重新扫描设备眼睛屏幕上的二维码，再从扫出来的链接进入。'
    : '没有找到访问凭证。请用手机相机扫描设备眼睛屏幕上的二维码，从扫出来的链接进入这个页面。';
});
const joinedHint = computed(() => `已加入「${props.page.wifi.joinSsid.value}」${status.value?.ipv4 ? `，地址 ${status.value.ipv4}` : ''}`);
</script>

<template>
  <div class="stack body">
    <!-- No usable token: never fall back to an address form here. -->
    <MdCard v-if="gate === 'auth' || gate === 'pairing'">
      <EmptyState icon="qr_code" :title="page.deviceBuild ? '请重新扫描设备上的二维码' : '需要重新认证'" :hint="authHint" action-text="再试一次" @action="page.link.retry()" />
    </MdCard>

    <MdCard v-else-if="gate === 'unreachable'">
      <EmptyState tone="error" title="连接不上设备" hint="请确认手机已连接到设备的配网热点（或与设备在同一个 WiFi 下），然后重试。" action-text="重试" @action="page.link.retry()" />
    </MdCard>

    <MdCard v-else-if="gate === 'connecting'" title="正在连接设备">
      <Skeleton :lines="3" />
    </MdCard>

    <template v-else>
      <MdCard v-if="page.wifi.phase.value === 'joining'">
        <WifiJoinNotice :ssid="page.wifi.joinSsid.value" :via-hotspot="page.wifi.viaHotspot.value" :link-dropped="page.wifi.linkDropped.value" />
        <div class="row actions"><MdButton variant="text" :disabled="!page.session.connected" @click="page.wifi.retry()">重新填写</MdButton></div>
      </MdCard>

      <MdCard v-else-if="page.wifi.phase.value === 'joined'">
        <EmptyState icon="check_circle" title="设备已连上 WiFi" :hint="joinedHint" action-text="进入主页" @action="page.goHome()" />
      </MdCard>

      <template v-else>
        <MdCard title="当前网络">
          <Skeleton v-if="page.wifi.statusBusy.value && !status" :lines="2" />
          <EmptyState v-else-if="!status" tone="error" compact title="读取网络状态失败" :hint="page.wifi.statusError.value ?? undefined" action-text="重试" @action="page.wifi.refreshStatus()" />
          <div v-else class="status">
            <span class="orb"><UiIcon name="wifi" :size="24" /></span>
            <div class="status-body">
              <div class="status-title">{{ status.ssid ?? '还没有配置 WiFi' }}</div>
              <div class="muted status-sub">
                <template v-if="status.ipv4">地址 {{ status.ipv4 }}</template>
                <template v-else-if="status.state === 'ap_provision'">设备正在用自己的热点等待配网</template>
                <template v-else-if="status.error">{{ status.error }}</template>
                <template v-else>{{ status.ifname ?? '—' }}</template>
              </div>
            </div>
            <span class="tag" :class="page.stateTone.value">{{ page.stateLabel.value }}</span>
          </div>
        </MdCard>

        <p v-if="page.wifi.joinError.value" class="notice err" role="alert"><UiIcon name="error" :size="18" /> <span>{{ page.wifi.joinError.value }}请检查密码后再试一次。</span></p>
        <p v-if="page.linkLost.value" class="notice" role="status"><UiIcon name="sync" :size="18" /> <span>与设备的连接中断了，正在重连…</span></p>

        <MdCard title="选择要加入的 WiFi">
          <WifiSetupForm :setup="page.wifi" submit-text="让设备连接这个 WiFi" />
        </MdCard>

        <div v-if="status?.state === 'sta_online'" class="row actions"><MdButton variant="text" @click="page.goHome()">跳过，进入主页</MdButton></div>
      </template>
    </template>
  </div>
</template>

<style scoped>
.body { gap: 14px; }
.status { display: flex; align-items: center; gap: 12px; }
.orb { width: 44px; height: 44px; border-radius: 50%; display: grid; place-items: center; flex: none; background: rgba(var(--md-primary-rgb), 0.14); color: var(--md-primary); }
.status-body { flex: 1; min-width: 0; }
.status-title { font: 600 15.5px var(--font-title); color: var(--md-on-surface); word-break: break-all; }
.status-sub { font-size: 12.5px; margin-top: 2px; }
.notice { display: flex; align-items: flex-start; gap: 8px; margin: 0; padding: 10px 14px; border-radius: var(--radius-m); font-size: 13.5px; line-height: 1.5; color: var(--md-on-surface); background: color-mix(in srgb, var(--md-warning) 16%, var(--md-surface-container)); }
.notice.err { background: color-mix(in srgb, var(--md-error) 16%, var(--md-surface-container)); }
.notice .ui-icon { flex: none; margin-top: 1px; }
.actions { justify-content: flex-end; margin-top: 8px; }
</style>
