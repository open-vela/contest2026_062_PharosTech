<script setup lang="ts">
/* Tablet: segmented tabs (页面 / 权限) on top of one column. */
import { ref } from 'vue';
import { NyaUiPage } from '@nyabula/nyaui-vue';
import { EmptyState, MdButton, SegmentedTabs, Skeleton, UiIcon } from '@nyabula/ui';
import PluginPermissionPanel from './PluginPermissionPanel.vue';
import { usePluginPageView } from './PluginPageView.logic';
import { stateLabel, stateTone } from './plugins.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPageView(props);
const { page, perms } = v;
const tab = ref('page');
const tabs = [
  { id: 'page', label: '页面', icon: 'widgets' },
  { id: 'perms', label: '权限', icon: 'shield', badge: perms.summary.value.missing || undefined },
];
</script>

<template>
  <div class="page narrow plugin-tablet">
    <div class="row between">
      <div class="row">
        <UiIcon :name="page.plugin.value?.icon ?? 'extension'" :size="24" />
        <div>
          <h2 class="page-title">{{ page.title.value }}</h2>
          <p class="page-sub">
            <span v-if="page.plugin.value?.version" class="mono">v{{ page.plugin.value.version }} · </span>
            <span class="tag" :class="stateTone(page.plugin.value?.state)">{{ stateLabel(page.plugin.value?.state) }}</span>
          </p>
        </div>
      </div>
      <MdButton variant="icon" :disabled="page.loading.value" aria-label="重新加载" @click="page.retry"><UiIcon name="refresh" :size="20" /></MdButton>
    </div>

    <SegmentedTabs v-model="tab" :items="tabs" stretch />

    <template v-if="tab === 'page'">
      <EmptyState
        v-if="page.errorText.value && !page.tree.value"
        tone="error"
        icon="extension"
        title="插件页面加载失败"
        :hint="page.errorText.value"
        action-text="重试"
        @action="page.retry"
      />
      <Skeleton v-else-if="!page.tree.value" :lines="8" />
      <div v-else class="stage">
        <p v-if="!page.session.canControl" class="muted readonly">当前为访客身份，页面只读</p>
        <NyaUiPage :tree="page.tree.value" :busy="page.busy.value" @command="page.onCommand" />
      </div>
    </template>
    <div v-else class="stack">
      <PluginPermissionPanel :id="v.id.value" />
      <MdButton variant="text" @click="v.openPermissions(v.id.value)">查看权限说明</MdButton>
    </div>
  </div>
</template>

<style scoped>
.plugin-tablet { display: flex; flex-direction: column; gap: 14px; }
.stage { background: var(--md-surface-container-low); border-radius: var(--radius-l, 18px); padding: 16px; }
.readonly { font-size: 12.5px; margin: 0 0 10px; }
</style>
