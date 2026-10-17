<script setup lang="ts">
/* Tablet: landscape = two columns (stats + devices | claim + account + market);
 * portrait = segmented tabs over a single column. */
import { computed, inject, ref } from 'vue';
import { MdCard, SegmentedTabs } from '@nyabula/ui';
import AuthForm from './AuthForm.vue';
import AccountOverview from './AccountOverview.vue';
import DeviceList from './DeviceList.vue';
import ClaimForm from './ClaimForm.vue';
import CloudMarketCard from './CloudMarketCard.vue';
import AccountCard from './AccountCard.vue';
import { ACCOUNT_SECTIONS, useAccountPage } from './account.logic';
import type { useFormFactor } from '../../composables/useFormFactor';

const page = useAccountPage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const portrait = computed(() => ff.orientation.value === 'portrait');
const tab = ref('overview');
</script>

<template>
  <div v-if="!page.account.loggedIn" class="page narrow auth-page">
    <h1 class="page-title">Nyabula Cloud</h1>
    <p class="page-sub">登录后可远程连接已认领的设备、查看在线统计。</p>
    <AuthForm :page="page" />
  </div>

  <div v-else-if="!portrait" class="acct-tablet">
    <section class="col">
      <h1 class="page-title">概览</h1>
      <AccountOverview :page="page" />
      <p class="section-title">我的设备</p>
      <DeviceList :page="page" />
    </section>
    <section class="col side">
      <AccountCard :page="page" />
      <MdCard title="认领设备"><ClaimForm :page="page" /></MdCard>
      <CloudMarketCard />
    </section>
  </div>

  <div v-else class="page acct-portrait">
    <SegmentedTabs v-model="tab" :items="ACCOUNT_SECTIONS" stretch />
    <div class="stack body">
      <template v-if="tab === 'overview'">
        <AccountOverview :page="page" />
        <AccountCard :page="page" />
      </template>
      <DeviceList v-else-if="tab === 'devices'" :page="page" @claim="tab = 'claim'" />
      <MdCard v-else-if="tab === 'claim'" title="认领设备"><ClaimForm :page="page" @done="tab = 'devices'" /></MdCard>
      <CloudMarketCard v-else />
    </div>
  </div>
</template>

<style scoped>
.auth-page { padding-top: 40px; max-width: 480px; }
.acct-tablet {
  display: grid;
  grid-template-columns: minmax(0, 1.4fr) minmax(300px, 1fr);
  gap: 18px;
  padding: 18px 20px 40px;
}
.col { min-width: 0; }
.side { display: flex; flex-direction: column; gap: 14px; }
.acct-portrait { padding-top: 14px; }
.body { margin-top: 14px; gap: 14px; }
</style>
