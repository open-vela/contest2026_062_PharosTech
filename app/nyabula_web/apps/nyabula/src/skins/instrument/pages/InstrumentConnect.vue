<script setup lang="ts">
/* Instrument connect page on the shared connect logic (useConnectPage):
 * dot-matrix header with link readouts, known devices and the add-device
 * forms (LAN / advanced URL / token / cloud account devices) reused from
 * the classic page. Phone keeps the classic flow: list first, the form in
 * a bottom sheet that auto-opens when nothing is known or on re-auth. */
import { computed, inject, onMounted, ref } from 'vue';
import { BottomSheet, UiIcon } from '@nyabula/ui';
import { useConnectPage } from '../../../views/connect/connect.logic';
import KnownDeviceList from '../../../views/connect/KnownDeviceList.vue';
import ConnectForm from '../../../views/connect/ConnectForm.vue';
import type { useFormFactor } from '../../../composables/useFormFactor';
import InsPanel from '../parts/InsPanel.vue';
import DotMatrix from '../parts/DotMatrix.vue';

const page = useConnectPage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const phone = computed(() => ff.formFactor.value === 'phone');
const sheet = ref(false);
onMounted(() => {
  if (page.account.loggedIn) void page.account.refreshDevices().catch(() => undefined);
  if (phone.value && (!page.known.value.length || page.reauth)) sheet.value = true;
});
const pad = (n: number) => String(n).padStart(2, '0');
const linkTone = computed(() => (page.session.connected ? 'ok' : page.connecting.value ? 'busy' : 'off'));
</script>

<template>
  <div class="ins-connect" :class="{ phone }">
    <header class="hero">
      <DotMatrix text="NYABULA" :dot="phone ? 4 : 6" :gap="phone ? 1.5 : 2.2" class="hero-word" />
      <div class="hero-text">
        <p class="kicker">LINK // 连接</p>
        <h2 class="hero-title">连接你的 Nyabula</h2>
        <p class="hero-sub">局域网直连或经 Nyabula Cloud 远程接入。同一工作区，两种方式。</p>
      </div>
      <dl class="ro">
        <div><dt>KNOWN</dt><dd>{{ pad(page.known.value.length) }}</dd></div>
        <div><dt>CLOUD</dt><dd>{{ page.account.loggedIn ? pad(page.cloudDevices.value.length) : '--' }}</dd></div>
        <div><dt>LINK</dt><dd class="link" :class="linkTone"><i class="led" />{{ linkTone === 'ok' ? 'UP' : linkTone === 'busy' ? 'SYNC' : 'IDLE' }}</dd></div>
      </dl>
    </header>

    <template v-if="!phone">
      <div class="cols">
        <InsPanel idx="01" code="RECENT" label="最近的设备">
          <KnownDeviceList :page="page" grid />
        </InsPanel>
        <InsPanel idx="02" code="ADD" :label="page.reauth ? '重新认证' : '添加设备'" class="form-panel">
          <ConnectForm :page="page" />
        </InsPanel>
      </div>
    </template>
    <template v-else>
      <InsPanel idx="01" code="RECENT" label="最近的设备">
        <KnownDeviceList :page="page" />
      </InsPanel>
      <div class="fab-row">
        <button class="fab" @click="sheet = true"><UiIcon name="add" :size="18" /> 添加设备</button>
      </div>
      <BottomSheet :open="sheet" :title="page.reauth ? '重新认证' : '添加设备'" max-height="92vh" @close="sheet = false">
        <ConnectForm :page="page" />
      </BottomSheet>
    </template>
  </div>
</template>

<style scoped>
.ins-connect { padding: 20px 24px 40px; max-width: 1400px; margin: 0 auto; display: flex; flex-direction: column; gap: 14px; }
.ins-connect.phone { padding: 12px 12px 100px; }
.hero {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr) auto;
  align-items: center;
  gap: 24px;
  padding: 20px 22px;
  border-radius: 4px;
  background: var(--md-surface-container);
  box-shadow: inset 0 0 0 1px var(--md-outline-variant);
  color: var(--md-on-surface);
  animation: hero-in 0.36s steps(4, jump-none) both;
}
.phone .hero { grid-template-columns: minmax(0, 1fr); gap: 14px; padding: 16px; }
.hero-word { max-width: 100%; height: auto; }
.kicker { margin: 0 0 4px; font: 600 10.5px var(--ins-mono); letter-spacing: 0.2em; color: var(--md-primary); }
.hero-title { font: 400 22px var(--font-title); margin: 0 0 4px; }
.hero-sub { margin: 0; font-size: 13px; color: var(--md-on-surface-variant); }
.ro { display: grid; grid-template-columns: repeat(3, auto); gap: 1px; margin: 0; background: var(--md-outline-variant); border: 1px solid var(--md-outline-variant); }
.phone .ro { grid-template-columns: repeat(3, minmax(0, 1fr)); }
.ro > div { padding: 8px 14px; background: var(--md-surface-container); }
.ro dt { font: 600 9.5px var(--ins-mono); letter-spacing: 0.16em; color: var(--md-outline); }
.ro dd { margin: 2px 0 0; font: 600 18px var(--ins-mono); }
.ro dd.link { display: flex; align-items: center; gap: 6px; font-size: 13px; padding-top: 4px; }
.led { width: 7px; height: 7px; border-radius: 50%; background: var(--md-outline); }
.link.ok { color: var(--md-primary); }
.link.ok .led { background: var(--md-primary); box-shadow: var(--ins-led-glow); }
.link.busy .led { background: var(--md-warning); animation: blink 0.9s steps(2, jump-none) infinite; }
.cols { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(360px, 1fr); gap: 14px; align-items: start; }
@media (max-width: 1000px) { .cols { grid-template-columns: 1fr; } }
.form-panel :deep(.md-card) { background: var(--md-surface); }
.fab-row { position: fixed; left: 0; right: 0; bottom: calc(var(--shell-bottom) + 16px + var(--safe-b)); display: flex; justify-content: center; pointer-events: none; }
.fab {
  pointer-events: auto;
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 12px 22px;
  border: none;
  border-radius: 3px;
  background: var(--md-primary);
  color: var(--md-on-primary);
  font: 600 13px var(--ins-mono);
  letter-spacing: 0.08em;
  box-shadow: inset 0 -3px 0 rgba(0, 0, 0, 0.25), 0 10px 24px -10px rgba(0, 0, 0, 0.6);
  cursor: pointer;
}
.fab:active { transform: translateY(1px); }
@keyframes hero-in { from { opacity: 0; } to { opacity: 1; } }
@keyframes blink { 50% { opacity: 0.2; } }
</style>
