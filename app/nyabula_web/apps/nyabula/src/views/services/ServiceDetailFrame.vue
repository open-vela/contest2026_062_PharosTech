<script setup lang="ts">
/* Shared body of the feature detail page: header (icon / name / blurb with
 * device status on the right), lazily loaded feature component with a
 * skeleton while loading, and an EmptyState for unknown types. Variants
 * wrap this with their own layout (desktop adds the eye preview in the
 * context panel). Back navigation is handled by the shell. */
import { EmptyState, NkHeader, Skeleton } from '@nyabula/ui';
import { provide, shallowRef, type VNode } from 'vue';
import { featureHeaderKey } from './featureHeader';
import type { FormFactor } from '../../composables/useFormFactor';
import type { useServiceDetail } from './serviceDetail.logic';

const props = defineProps<{ detail: ReturnType<typeof useServiceDetail>; type: string; ff: FormFactor }>();
const { def, component, planned } = props.detail;
const header = shallowRef<(() => VNode) | null>(null);
provide(featureHeaderKey, (render) => {
  header.value = render;
  return () => { if (header.value === render) header.value = null; };
});
</script>

<template>
  <div class="frame">
    <template v-if="def">
      <!-- Planned: the page is shown, with a line saying what it is. -->
      <p v-if="planned" class="preview" role="note">
        <strong>{{ def.label }} · 规划中</strong>
        {{ (def.needs ?? '等待硬件与驱动接入') + '。以下为界面预览，设备端尚未接入。' }}
      </p>
      <component :is="header" v-if="header" />
      <NkHeader v-else :icon="def.icon" :title="def.label" :subtitle="def.blurb" />
      <Suspense>
        <component :is="component" v-if="component" :key="type" :type="type" :ff="ff" />
        <template #fallback>
          <div class="loading">
            <Skeleton height="160px" radius="var(--radius-l)" />
            <Skeleton :lines="3" />
          </div>
        </template>
      </Suspense>
    </template>
    <EmptyState v-else icon="help" title="未知功能" :hint="'没有名为「' + type + '」的功能，可能已下线。'" action-text="返回功能列表" @action="detail.back()" />
  </div>
</template>

<style scoped>
.frame { display: flex; flex-direction: column; gap: 18px; }
.loading { display: flex; flex-direction: column; gap: 14px; }
.preview {
  margin: 0;
  padding: 12px 14px;
  border-radius: var(--radius-l);
  border: 1px dashed var(--md-outline-variant);
  background: var(--md-surface-container);
  color: var(--md-on-surface-variant);
  font-size: 13px;
  line-height: 1.55;
}
.preview strong { display: block; color: var(--md-on-surface); margin-bottom: 2px; }
</style>
