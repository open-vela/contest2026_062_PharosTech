<script setup lang="ts">
/* Pairing feature: an explanation plus the live connection status. The device
 * decides by itself when to put the pairing QR on its eyes (scene `qr`); the
 * panel never sees or relays the access token, so there is nothing to push
 * from here. The page only reflects that the QR is up. */
import { computed } from 'vue';
import { NkBanner, NkKeyValue, NkListSection, NkRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();

const showing = computed(() => eye.activeScene === 'qr');
const ROLE_LABEL: Record<string, string> = { owner: '主人', family: '家人', guest: '访客' };
const STATE_LABEL: Record<string, string> = {
  idle: '未连接',
  connecting: '连接中',
  authenticating: '验证中',
  'pairing-required': '需要配对',
  connected: '已连接',
  error: '连接失败',
};
const stateLabel = computed(() => STATE_LABEL[session.state] ?? session.state);
const roleLabel = computed(() => (session.role ? ROLE_LABEL[session.role] ?? session.role : '未配对'));
const needsPairing = computed(() => session.state === 'pairing-required');
const subtitle = computed(() => (showing.value ? '设备正在眼睛上显示二维码' : `${stateLabel.value} · ${roleLabel.value}`));

const kv = computed(() => [
  { key: '连接状态', value: stateLabel.value, tone: session.connected ? ('ok' as const) : needsPairing.value ? ('warn' as const) : ('default' as const) },
  { key: '我的身份', value: roleLabel.value, tone: session.isOwner ? ('primary' as const) : ('default' as const) },
  { key: '设备', value: session.device?.name ?? session.deviceKey ?? '—' },
]);

const STEPS = [
  { icon: 'qr_code', title: '设备自己显示二维码', sub: '首次开机或需要接入新成员时，猫眼会显示二维码，无需在这里操作' },
  { icon: 'smartphone', title: '用新设备扫码', sub: '手机或平板扫描猫眼上的二维码，即可打开并接入 Nyabula' },
  { icon: 'check_circle', title: '完成接入', sub: '主人可以在「访问与成员」里为新成员分配家人或访客身份' },
];
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="qr_code" title="配对" :subtitle="subtitle" :tone="showing ? 'ok' : needsPairing ? 'warn' : 'default'" />
    <div class="grid">
      <section class="card center">
        <div class="stage" :class="{ on: showing }">
          <UiIcon name="qr_code" :size="64" />
          <div class="stage-text">{{ showing ? '二维码显示中，请查看猫眼' : '二维码只在设备屏幕显示' }}</div>
          <div class="muted small">二维码里含访问凭据，出于安全考虑由设备自己生成和显示，控制台不经手、也无法代为显示。</div>
        </div>
        <NkBanner v-if="needsPairing" tone="warn" text="当前连接需要配对，请扫描猫眼上的二维码。" />
      </section>
      <section class="card">
        <NkKeyValue :items="kv" />
        <NkListSection title="配对步骤">
          <NkRow v-for="(s, i) in STEPS" :key="i" :icon="s.icon" :title="`${i + 1}. ${s.title}`" :sub="s.sub" />
        </NkListSection>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.center > * { width: 100%; }
.stage {
  display: flex; flex-direction: column; align-items: center; gap: 8px; padding: 28px 16px; text-align: center;
  border-radius: var(--radius-l); background: var(--md-surface-container-high); color: var(--md-on-surface-variant);
  transition: background var(--dur-medium) var(--ease-standard), color var(--dur-medium) var(--ease-standard);
}
.stage.on { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.stage-text { font-size: 18px; font-weight: 700; }
.small { font-size: 12px; margin: 0; }
</style>
