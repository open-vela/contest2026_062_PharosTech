<script setup lang="ts">
/* The two halves of the feature list, shared by the phone / tablet / desktop
 * pages (each passes its own tile grid): "已实现" works against the device
 * today, "规划中" waits for hardware or driver work and is display-only. */
import { NkListSection } from '@nyabula/ui';
import FeatureCard from './FeatureCard.vue';
import type { useServicesPage } from './services.logic';
import type { FormFactor } from '../../composables/useFormFactor';

defineProps<{
  page: ReturnType<typeof useServicesPage>;
  ff: FormFactor;
  /** grid-template-columns of the tile grid. */
  columns: string;
}>();
</script>

<template>
  <section class="stage" aria-labelledby="stage-ready">
    <header class="stage-head">
      <h2 id="stage-ready" class="stage-title">已实现</h2>
      <span class="stage-count">{{ page.readyCount }} 项 · 已接入设备，可显示到猫眼</span>
    </header>
    <NkListSection v-for="g in page.readyGroups" :key="g.group" :title="g.group" :card="false">
      <div class="tiles" :style="{ gridTemplateColumns: columns }">
        <FeatureCard
          v-for="f in g.items"
          :key="f.type"
          :def="f"
          :active="page.isActive(f)"
          :sub="page.subFor(f)"
          :ff="ff"
          :payload="page.isActive(f) ? page.livePayload.value : null"
          @open="page.open(f)"
        />
      </div>
    </NkListSection>
  </section>

  <section class="stage planned" aria-labelledby="stage-planned">
    <header class="stage-head">
      <h2 id="stage-planned" class="stage-title">规划中</h2>
      <span class="stage-count">{{ page.plannedCount }} 项 · 等待硬件或驱动接入，暂不可用</span>
    </header>
    <NkListSection v-for="g in page.plannedGroups" :key="g.group" :title="g.group" :card="false">
      <div class="tiles" :style="{ gridTemplateColumns: columns }">
        <FeatureCard v-for="f in g.items" :key="f.type" :def="f" :active="false" :sub="page.subFor(f)" :ff="ff" :payload="null" />
      </div>
    </NkListSection>
  </section>
</template>

<style scoped>
.stage { display: flex; flex-direction: column; gap: 14px; }
.stage.planned { margin-top: 10px; padding-top: 18px; border-top: 1px solid var(--md-outline-variant); }
.stage-head { display: flex; align-items: baseline; flex-wrap: wrap; gap: 4px 10px; }
.stage-title { margin: 0; font: 700 17px var(--font-body); color: var(--md-on-surface); }
.stage.planned .stage-title { color: var(--md-on-surface-variant); }
.stage-count { font-size: 12.5px; color: var(--md-on-surface-variant); }
.tiles { display: grid; gap: 12px; align-items: start; }
</style>
