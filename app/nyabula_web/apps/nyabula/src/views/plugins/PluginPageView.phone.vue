<script setup lang="ts">
/* Phone: page fills the screen (AppBar already provides back); a fixed
 * bottom button opens the permission sheet. */
import { ref } from 'vue';
import { NyaUiPage } from '@nyabula/nyaui-vue';
import { BottomSheet, EmptyState, MdButton, Skeleton, UiIcon } from '@nyabula/ui';
import PluginPermissionPanel from './PluginPermissionPanel.vue';
import { usePluginPageView } from './PluginPageView.logic';

const props = defineProps<{ key?: string; id: string }>();
const v = usePluginPageView(props);
const { page, perms } = v;
const sheet = ref(false);
</script>

<template>
  <div class="plugin-phone">
    <div class="row between head">
      <span class="row title"><UiIcon :name="page.plugin.value?.icon ?? 'extension'" :size="20" /> {{ page.title.value }}</span>
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
    <template v-else>
      <p v-if="!page.session.canControl" class="muted readonly">当前为访客身份，页面只读</p>
      <NyaUiPage :tree="page.tree.value" :busy="page.busy.value" @command="page.onCommand" />
    </template>

    <div class="bottom-bar">
      <MdButton variant="tonal" class="perm-btn" @click="sheet = true">
        <span class="row" style="gap: 6px">
          <UiIcon name="shield" :size="18" />
          权限 {{ perms.summary.value.granted }}/{{ perms.summary.value.total }}
          <span v-if="perms.summary.value.missing" class="tag warn">缺 {{ perms.summary.value.missing }}</span>
        </span>
      </MdButton>
    </div>

    <BottomSheet :open="sheet" title="插件权限" @close="sheet = false">
      <PluginPermissionPanel :id="v.id.value" />
      <MdButton variant="text" class="more" @click="sheet = false; v.openPermissions(v.id.value)">查看权限说明</MdButton>
    </BottomSheet>
  </div>
</template>

<style scoped>
.plugin-phone { display: flex; flex-direction: column; gap: 12px; padding: 4px 14px calc(var(--shell-bottom) + 76px); }
.head { margin-bottom: 2px; }
.title { font: 600 16px var(--font-body); min-width: 0; }
.readonly { font-size: 12.5px; margin: 0; }
.bottom-bar {
  position: fixed;
  left: 14px;
  right: 14px;
  bottom: calc(var(--shell-bottom) + 12px);
  z-index: 5;
}
.perm-btn { width: 100%; }
.more { width: 100%; margin-top: 10px; }
</style>
