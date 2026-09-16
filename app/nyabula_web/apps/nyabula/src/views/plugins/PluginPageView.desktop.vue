<script setup lang="ts">
/* Desktop: NyaUI page centered; context panel = permission switches +
 * collapsible developer raw-tree JSON. */
import { NyaUiPage } from '@nyabula/nyaui-vue';
import { EmptyState, MdButton, MdCard, Skeleton, UiIcon } from '@nyabula/ui';
import ContextSlot from '../../components/ContextSlot.vue';
import PluginPermissionPanel from './PluginPermissionPanel.vue';
import { usePluginPageView } from './PluginPageView.logic';
import { stateLabel, stateTone } from './plugins.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPageView(props);
const { page, perms } = v;
</script>

<template>
  <div class="page narrow plugin-page">
    <div class="row between head">
      <div class="row">
        <span class="icon"><UiIcon :name="page.plugin.value?.icon ?? 'extension'" :size="22" /></span>
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

    <ContextSlot>
      <MdCard title="权限">
        <PluginPermissionPanel :id="v.id.value" compact />
        <p class="muted small">已授 {{ perms.summary.value.granted }}/{{ perms.summary.value.total }}</p>
        <MdButton variant="text" @click="v.openPermissions(v.id.value)">查看说明</MdButton>
      </MdCard>
      <MdCard title="开发模式">
        <button class="dev-toggle" @click="v.devOpen.value = !v.devOpen.value">
          <UiIcon :name="v.devOpen.value ? 'expand_less' : 'expand_more'" :size="18" />
          <span>原始组件树 JSON</span>
          <span class="muted small">patch #{{ v.patchCount.value }}</span>
        </button>
        <pre v-if="v.devOpen.value" class="raw mono">{{ page.rawJson.value || '（尚未加载）' }}</pre>
      </MdCard>
    </ContextSlot>
  </div>
</template>

<style scoped>
.head { align-items: flex-start; margin-bottom: 4px; }
.icon {
  width: 44px;
  height: 44px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-m);
  background: var(--md-primary-container);
  color: var(--md-on-primary-container);
}
.stage {
  background: var(--md-surface-container-low);
  border-radius: var(--radius-l, 18px);
  padding: 18px;
}
.readonly { font-size: 12.5px; margin: 0 0 10px; }
.small { font-size: 12px; margin: 10px 0 4px; }
.dev-toggle {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  border: none;
  background: transparent;
  color: var(--md-on-surface);
  font: 600 13px var(--font-body);
  padding: 0;
  cursor: pointer;
}
.dev-toggle span:nth-child(2) { flex: 1; text-align: left; }
.raw {
  margin: 10px 0 0;
  max-height: 46vh;
  overflow: auto;
  font-size: 11.5px;
  line-height: 1.45;
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface-variant);
  border-radius: var(--radius-s);
  padding: 10px;
  white-space: pre;
}
</style>
