<script setup lang="ts">
/* Desktop dashboard: 12-col grid. Row 1: device hero (7) + eye preview (5).
 * Row 2: quick actions + ask (7) | attention + plugins (5). Row 3: widgets
 * full width. Activity feed lives in the shell context panel. */
import { inject } from 'vue';
import ContextSlot from '../../components/ContextSlot.vue';
import WidgetGrid from '../../components/WidgetGrid.vue';
import type { useFormFactor } from '../../composables/useFormFactor';
import { useHomePage } from './home.logic';
import DeviceHeroCard from './DeviceHeroCard.vue';
import EyePreviewCard from './EyePreviewCard.vue';
import QuickActions from './QuickActions.vue';
import AttentionList from './AttentionList.vue';
import PluginsSummaryCard from './PluginsSummaryCard.vue';
import ActivityFeed from './ActivityFeed.vue';
import AgentQuickAsk from './AgentQuickAsk.vue';

const page = useHomePage();
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
</script>

<template>
  <div class="home-desktop">
    <div class="col main">
      <DeviceHeroCard :page="page" v-reveal />
      <section v-reveal="60">
        <p class="section-title">快捷操作</p>
        <QuickActions :page="page" :columns="3" />
      </section>
      <AgentQuickAsk :page="page" v-reveal="120" />
      <WidgetGrid :form-factor="ff.formFactor.value" v-reveal="160" />
    </div>
    <div class="col side">
      <EyePreviewCard :page="page" height="240px" v-reveal="40" />
      <AttentionList :page="page" v-reveal="100" />
      <PluginsSummaryCard :page="page" v-reveal="140" />
    </div>
    <ContextSlot>
      <ActivityFeed :page="page" :limit="12" />
    </ContextSlot>
  </div>
</template>

<style scoped>
.home-desktop {
  display: grid;
  grid-template-columns: minmax(0, 7fr) minmax(320px, 5fr);
  gap: 20px;
  padding: 20px 24px 40px;
  max-width: 1400px;
  margin: 0 auto;
}
.col { display: flex; flex-direction: column; gap: 18px; min-width: 0; }
</style>
