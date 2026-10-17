<script setup lang="ts">
/* Things that need the user: pairing, cloud link, missing grants, battery. */
import { MdCard, UiIcon } from '@nyabula/ui';
import type { useHomePage } from './home.logic';

const props = defineProps<{ page: ReturnType<typeof useHomePage> }>();
const { attention } = props.page;
</script>

<template>
  <MdCard title="需要关注">
    <div v-if="attention.length" class="stack" style="gap: 8px">
      <button v-for="a in attention" :key="a.id" class="list-tile" @click="a.run()">
        <span class="tile-icon" :class="a.tone"><UiIcon :name="a.icon" :size="20" /></span>
        <span class="tile-body"><span class="tile-title">{{ a.title }}</span><span class="tile-sub">{{ a.sub }}</span></span>
        <span class="tile-trail"><UiIcon name="chevron_right" :size="18" /></span>
      </button>
    </div>
    <div v-else class="all-good">
      <UiIcon name="check_circle" :size="20" />
      <span>一切正常，没有待处理事项</span>
    </div>
  </MdCard>
</template>

<style scoped>
.tile-icon.warn { background: color-mix(in srgb, var(--md-warning) 25%, var(--md-surface-container-highest)); color: var(--md-warning); }
.tile-icon.err { background: color-mix(in srgb, var(--md-error) 18%, var(--md-surface-container-highest)); color: var(--md-error); }
.tile-icon.info { background: color-mix(in srgb, var(--md-tertiary) 22%, var(--md-surface-container-highest)); color: var(--md-tertiary); }
.all-good { display: flex; align-items: center; gap: 8px; color: var(--md-success); font-size: 13.5px; }
</style>
