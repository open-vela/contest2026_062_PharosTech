<script setup lang="ts">
/* Tablet: landscape = two columns (hero+actions+widgets | eye+attention+
 * plugins+feed); portrait = single stack with a horizontal action strip. */
import { computed, inject } from 'vue';
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
const portrait = computed(() => ff.orientation.value === 'portrait');
</script>

<template>
  <div class="home-tablet" :class="{ portrait }">
    <template v-if="!portrait">
      <div class="col">
        <DeviceHeroCard :page="page" />
        <section>
          <p class="section-title">快捷操作</p>
          <QuickActions :page="page" :columns="2" />
        </section>
        <AgentQuickAsk :page="page" />
        <WidgetGrid :form-factor="ff.formFactor.value" />
      </div>
      <div class="col">
        <EyePreviewCard :page="page" height="200px" />
        <AttentionList :page="page" />
        <PluginsSummaryCard :page="page" />
        <ActivityFeed :page="page" :limit="6" />
      </div>
    </template>
    <template v-else>
      <DeviceHeroCard :page="page" />
      <EyePreviewCard :page="page" height="260px" />
      <section>
        <p class="section-title">快捷操作</p>
        <QuickActions :page="page" horizontal />
      </section>
      <AttentionList :page="page" />
      <AgentQuickAsk :page="page" />
      <div class="two">
        <PluginsSummaryCard :page="page" />
        <ActivityFeed :page="page" :limit="5" />
      </div>
      <WidgetGrid :form-factor="ff.formFactor.value" />
    </template>
  </div>
</template>

<style scoped>
.home-tablet { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(300px, 1fr); gap: 18px; padding: 18px 20px 40px; }
.home-tablet.portrait { grid-template-columns: 1fr; display: flex; flex-direction: column; }
.col { display: flex; flex-direction: column; gap: 16px; min-width: 0; }
.two { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
</style>
