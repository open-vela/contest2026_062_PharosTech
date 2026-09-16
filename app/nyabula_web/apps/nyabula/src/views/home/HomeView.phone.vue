<script setup lang="ts">
/* Phone: compact hero, eye preview, horizontal quick-action strip,
 * attention, ask, then plugins / feed / widgets stacked. */
import { inject } from 'vue';
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
  <div class="home-phone">
    <DeviceHeroCard :page="page" compact />
    <EyePreviewCard :page="page" height="200px" />
    <section>
      <p class="section-title">快捷操作</p>
      <QuickActions :page="page" horizontal />
    </section>
    <AttentionList :page="page" />
    <AgentQuickAsk :page="page" />
    <PluginsSummaryCard :page="page" />
    <ActivityFeed :page="page" :limit="5" />
    <WidgetGrid :form-factor="ff.formFactor.value" />
  </div>
</template>

<style scoped>
.home-phone { display: flex; flex-direction: column; gap: 14px; padding: 8px 14px 32px; }
</style>
