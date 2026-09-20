<script setup lang="ts">
/* Shimmering placeholder block. */
defineProps<{ width?: string; height?: string; radius?: string; lines?: number }>();
</script>

<template>
  <div v-if="lines" class="sk-lines">
    <span v-for="i in lines" :key="i" class="sk" :style="{ width: i === lines ? '60%' : '100%', height: height ?? '14px', borderRadius: radius ?? '6px' }" />
  </div>
  <span v-else class="sk" :style="{ width: width ?? '100%', height: height ?? '16px', borderRadius: radius ?? 'var(--radius-s)' }" />
</template>

<style scoped>
.sk-lines { display: flex; flex-direction: column; gap: 8px; }
.sk {
  display: block;
  background: linear-gradient(90deg, var(--md-surface-container-high) 25%, var(--md-surface-container-highest) 50%, var(--md-surface-container-high) 75%);
  background-size: 200% 100%;
  animation: shimmer 1.4s ease-in-out infinite;
}
@keyframes shimmer { from { background-position: 200% 0; } to { background-position: -200% 0; } }
</style>
