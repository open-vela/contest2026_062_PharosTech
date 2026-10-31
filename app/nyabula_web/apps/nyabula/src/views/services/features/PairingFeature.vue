<script setup lang="ts">
/* Pairing feature. Shows how pairing works and lets the user put the pairing
 * code on the device eye (full style) or hide it. The code itself is only
 * ever rendered on the device screen, never here. Status comes from the
 * session (role / state). */
import { computed } from 'vue';
import { NkActionBar, NkBanner, NkKeyValue, NkListSection, NkRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const session = useSessionStore();

const showing = computed(() => eye.activeScene === props.type);
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
const subtitle = computed(() => (showing.value ? '配对码正在眼睛上显示' : `${stateLabel.value} · ${roleLabel.value}`));

const kv = computed(() => [
  { key: '连接状态', value: stateLabel.value, tone: session.connected ? ('ok' as const) : needsPairing.value ? ('warn' as const) : ('default' as const) },
  { key: '我的身份', value: roleLabel.value, tone: session.isOwner ? ('primary' as const) : ('default' as const) },
  { key: '设备', value: session.device?.name ?? session.deviceKey ?? '—' },
]);

const STEPS = [
  { icon: 'qr_code', title: '在眼睛上显示配对码', sub: '点击下方按钮，设备屏幕会显示一组配对码' },
  { icon: 'smartphone', title: '在新设备输入配对码', sub: '用手机或电脑打开 Nyabula，输入屏幕上的配对码' },
  { icon: 'check_circle', title: '完成配对', sub: '主人可以为新成员分配家人或访客身份' },
];

function show(): void {
  void eye.setScene(props.type, 'full');
}
function hide(): void {
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="qr_code" title="配对" :subtitle="subtitle" :tone="showing ? 'ok' : needsPairing ? 'warn' : 'default'" />
    <div class="grid">
      <section class="card center">
        <div class="stage" :class="{ on: showing }">
          <UiIcon name="qr_code" :size="64" />
          <div class="stage-text">{{ showing ? '配对码显示中' : '配对码只在设备屏幕显示' }}</div>
          <div class="muted small">出于安全考虑，此处不会显示配对码，请查看设备眼睛。</div>
        </div>
        <NkBanner v-if="needsPairing" tone="warn" text="当前连接需要配对，请在设备上显示配对码后输入。" />
        <NkActionBar
          :primary-text="showing ? '已在眼睛上显示' : '在眼睛上显示配对码'"
          primary-icon="visibility"
          secondary-text="隐藏"
          secondary-icon="close"
          :disabled="showing || !session.canControl"
          @primary="show"
          @secondary="hide"
        />
        <p v-if="!session.canControl" class="muted small">只有主人或家人可以显示配对码。</p>
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
