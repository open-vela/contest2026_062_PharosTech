<script setup lang="ts">
/* Fallback for a DEVICE_SECTIONS entry that has no component in
 * device.logic.ts BUILTIN yet (storage / update / logs are real sections
 * now: StorageSection, UpdateSection, LogsSection). */
import { EmptyState, MdCard } from '@nyabula/ui';

defineProps<{ section?: string }>();
</script>

<template>
  <div class="stack">
    <MdCard>
      <EmptyState icon="auto_awesome" title="预留" hint="此段落尚未定义。" compact />
      <div class="row" style="justify-content: center; margin-bottom: 8px"><span class="contract-only">契约预留 · 设备端尚未实现对应 NyaLink 主题</span></div>
    </MdCard>
  </div>
</template>
