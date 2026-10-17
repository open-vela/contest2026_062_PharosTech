<script setup lang="ts">
/* Desktop/tablet: hero + two columns (known devices grid | forms). */
import { onMounted } from 'vue';
import { UiIcon, NyabulaLogo } from '@nyabula/ui';
import { useConnectPage } from './connect.logic';
import KnownDeviceList from './KnownDeviceList.vue';
import ConnectForm from './ConnectForm.vue';

const page = useConnectPage();
onMounted(() => {
  if (page.account.loggedIn) void page.account.refreshDevices().catch(() => undefined);
});
</script>

<template>
  <div class="connect-desktop page">
    <header class="hero" v-reveal>
      <div class="hero-mark"><NyabulaLogo :size="64" /></div>
      <div>
        <h2 class="page-title">连接你的 Nyabula</h2>
        <p class="page-sub">局域网直连或经 Nyabula Cloud 远程接入。同一工作区，两种方式。</p>
      </div>
    </header>
    <div class="cols">
      <section v-reveal="60">
        <p class="section-title">最近的设备</p>
        <KnownDeviceList :page="page" grid />
      </section>
      <section v-reveal="120">
        <p class="section-title">添加设备</p>
        <ConnectForm :page="page" />
      </section>
    </div>
  </div>
</template>

<style scoped>
.hero { display: flex; align-items: center; gap: 18px; margin-bottom: 24px; }
.hero .page-title { margin-bottom: 4px; }
.hero .page-sub { margin-bottom: 0; }
.hero-mark {
  width: 64px;
  height: 64px;
  border-radius: 20px;
  display: grid;
  place-items: center;
  border-radius: 14px;
  overflow: hidden;
}
.cols { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(360px, 1fr); gap: 28px; }
@media (max-width: 1000px) { .cols { grid-template-columns: 1fr; } }
</style>
