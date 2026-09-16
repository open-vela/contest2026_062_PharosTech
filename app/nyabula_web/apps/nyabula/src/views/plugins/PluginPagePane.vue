<script setup lang="ts">
/* Self-contained NyaUI page for one plugin: load / skeleton / error / retry
 * and command forwarding. Used by the page view variants and the tablet
 * master-detail preview. */
import { toRef } from 'vue';
import { NyaUiPage } from '@nyabula/nyaui-vue';
import { EmptyState, Skeleton } from '@nyabula/ui';
import { usePluginPage } from './plugins.logic';

const props = defineProps<{ id: string; holdRoute?: boolean }>();
const page = usePluginPage(toRef(props, 'id'), { holdRoute: props.holdRoute });
defineExpose({ page });
</script>

<template>
  <div class="pane">
    <EmptyState
      v-if="page.errorText.value && !page.tree.value"
      tone="error"
      icon="extension"
      title="插件页面加载失败"
      :hint="page.errorText.value"
      action-text="重试"
      @action="page.retry"
    />
    <Skeleton v-else-if="!page.tree.value" :lines="6" />
    <template v-else>
      <p v-if="!page.session.canControl" class="muted readonly">当前为访客身份，页面只读</p>
      <NyaUiPage :tree="page.tree.value" :busy="page.busy.value" @command="page.onCommand" />
    </template>
  </div>
</template>

<style scoped>
.pane { min-width: 0; }
.readonly { font-size: 12.5px; margin: 0 0 10px; }
</style>
