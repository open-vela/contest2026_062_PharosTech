<script setup lang="ts">
/* Account overview: GET /stats/overview cards. */
import { onMounted, ref } from 'vue';
import { MdCard } from '@nyabula/ui';
import { statsOverview, type Overview } from '../api/client';

const overview = ref<Overview | null>(null);
const error = ref<string | null>(null);

onMounted(async () => {
  try {
    overview.value = await statsOverview();
  } catch (e) {
    error.value = e instanceof Error ? e.message : String(e);
  }
});
</script>

<template>
  <div class="page">
    <h2 class="title">总览</h2>
    <p v-if="error" class="err">{{ error }}</p>
    <div class="grid">
      <MdCard title="设备总数">
        <span class="big">{{ overview?.devices ?? '—' }}</span>
      </MdCard>
      <MdCard title="在线设备">
        <span class="big ok">{{ overview?.online ?? '—' }}</span>
      </MdCard>
      <MdCard title="活跃客户端">
        <span class="big">{{ overview?.clients ?? '—' }}</span>
      </MdCard>
      <MdCard title="今日转发帧数">
        <span class="big">{{ overview?.framesToday?.toLocaleString() ?? '—' }}</span>
      </MdCard>
    </div>
  </div>
</template>

<style scoped>
.page {
  padding: 22px;
  max-width: 960px;
  margin: 0 auto;
}
.title {
  font-size: 22px;
  margin-bottom: 18px;
}
.grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
  gap: 16px;
}
.big {
  font: 600 34px var(--font-body);
  color: var(--md-on-surface);
}
.big.ok {
  color: var(--md-primary);
}
.err {
  color: var(--md-error);
  font-size: 13px;
  margin-bottom: 14px;
}
</style>
