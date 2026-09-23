<script setup lang="ts">
/* Reserved area for Cloud-only features (plugin market etc.). Contract is
 * defined but no backend endpoint exists yet: shown, never hidden. */
import { MdCard, UiIcon } from '@nyabula/ui';

const FEATURES = [
  { id: 'market', icon: 'store', title: '插件市场', sub: '浏览并一键安装社区插件到已认领的设备。Cloud 后端上线后此处接入。' },
  { id: 'sync', icon: 'cloud', title: '云端记忆与同步', sub: '跨设备同步表情偏好、对话记忆与场景配置。' },
];
</script>

<template>
  <MdCard title="Cloud 专属功能">
    <div v-for="f in FEATURES" :key="f.id" class="feat">
      <div class="feat-icon"><UiIcon :name="f.icon" :size="24" /></div>
      <div class="feat-body">
        <div class="row">
          <span class="feat-title">{{ f.title }}</span>
          <span class="contract-only">契约预留</span>
        </div>
        <p class="muted feat-sub">{{ f.sub }}</p>
      </div>
    </div>
  </MdCard>
</template>

<style scoped>
.feat { display: flex; gap: 14px; padding: 10px 0; border-top: 1px solid var(--md-outline-variant); }
.feat:first-of-type { border-top: none; padding-top: 0; }
.feat-icon {
  width: 44px;
  height: 44px;
  border-radius: 12px;
  display: grid;
  place-items: center;
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  flex: none;
}
.feat-title { font-weight: 600; font-size: 14.5px; color: var(--md-on-surface); }
.feat-sub { font-size: 12.5px; margin: 4px 0 0; }
</style>
