<script setup lang="ts">
defineProps<{ title?: string }>();
</script>

<template>
  <section class="md-card">
    <h3 v-if="title" class="card-title">{{ title }}</h3>
    <slot />
  </section>
</template>

<style scoped>
.md-card {
  background: var(--md-surface-container);
  border-radius: var(--radius-l);
  padding: 18px 20px;
  box-shadow: var(--md-elev-1);
}
.card-title {
  font-size: 15px;
  color: var(--md-on-surface-variant);
  margin-bottom: 12px;
  letter-spacing: 1px;
}
</style>
