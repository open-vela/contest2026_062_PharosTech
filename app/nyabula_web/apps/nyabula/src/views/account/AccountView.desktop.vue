<script setup lang="ts">
/* Desktop: left directory (概览 / 设备 / 认领 / 市场), content on the right,
 * identity card in the shell context panel. Signed out = centered auth. */
import { ref } from 'vue';
import { MdCard, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import AuthForm from './AuthForm.vue';
import AccountOverview from './AccountOverview.vue';
import DeviceList from './DeviceList.vue';
import ClaimForm from './ClaimForm.vue';
import CloudMarketCard from './CloudMarketCard.vue';
import AccountCard from './AccountCard.vue';
import { ACCOUNT_SECTIONS, useAccountPage, type AccountSection } from './account.logic';

const page = useAccountPage();
const section = ref<AccountSection>('overview');
</script>

<template>
  <div v-if="!page.account.loggedIn" class="page narrow auth-page">
    <h1 class="page-title">Nyabula Cloud</h1>
    <p class="page-sub">登录后可远程连接已认领的设备、查看在线统计。</p>
    <AuthForm :page="page" />
  </div>

  <div v-else class="acct-desktop">
    <nav class="dir">
      <button v-for="s in ACCOUNT_SECTIONS" :key="s.id" class="dir-item" :class="{ on: section === s.id }" @click="section = s.id">
        <UiIcon :name="s.icon" :size="20" />
        <span>{{ s.label }}</span>
        <span v-if="s.id === 'devices' && page.account.devices.length" class="tag">{{ page.account.devices.length }}</span>
      </button>
    </nav>

    <section class="content">
      <template v-if="section === 'overview'">
        <h1 class="page-title">概览</h1>
        <p class="page-sub">账号下所有设备的汇总。</p>
        <AccountOverview :page="page" />
        <p class="section-title">我的设备</p>
        <DeviceList :page="page" @claim="section = 'claim'" />
      </template>

      <template v-else-if="section === 'devices'">
        <h1 class="page-title">我的设备</h1>
        <p class="page-sub">在线设备可直接远程连接；统计页查看每日在线与帧数。</p>
        <DeviceList :page="page" @claim="section = 'claim'" />
      </template>

      <template v-else-if="section === 'claim'">
        <h1 class="page-title">认领设备</h1>
        <p class="page-sub">把设备绑定到当前账号，之后即可通过 Cloud 中继远程访问。</p>
        <MdCard class="claim-card"><ClaimForm :page="page" @done="section = 'devices'" /></MdCard>
      </template>

      <template v-else>
        <h1 class="page-title">市场</h1>
        <p class="page-sub">Cloud 独占能力，契约已定，后端接入后开放。</p>
        <CloudMarketCard />
      </template>
    </section>

    <ContextSlot>
      <AccountCard :page="page" />
    </ContextSlot>
  </div>
</template>

<style scoped>
.auth-page { padding-top: 48px; max-width: 460px; }
.acct-desktop {
  display: grid;
  grid-template-columns: 200px minmax(0, 1fr);
  gap: 24px;
  padding: 20px 24px 40px;
  max-width: 1200px;
  margin: 0 auto;
}
.dir { display: flex; flex-direction: column; gap: 4px; position: sticky; top: 20px; align-self: start; }
.dir-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px 14px;
  border-radius: var(--radius-m);
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 14px var(--font-body);
  text-align: left;
  cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast);
}
.dir-item span:nth-child(2) { flex: 1; }
.dir-item:hover { background: var(--md-surface-container); }
.dir-item.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.content { min-width: 0; }
.claim-card { max-width: 480px; }
</style>
