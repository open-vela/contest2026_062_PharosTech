<script setup lang="ts">
/* Snackbar stack. Place once in App. `bottom` offset lets phone shells lift
 * toasts above the bottom nav. */
import { useToastStore } from '../stores/toast';
import UiIcon from './UiIcon.vue';

defineProps<{ bottom?: string }>();
const toast = useToastStore();
const ICON: Record<string, string> = { info: 'info', ok: 'check_circle', warn: 'warning', error: 'error' };
</script>

<template>
  <Teleport to="body">
    <div class="toast-host" :style="{ bottom: bottom ?? '24px' }">
      <TransitionGroup name="toast">
        <div v-for="t in toast.items" :key="t.id" class="toast" :class="t.tone" role="status">
          <UiIcon :name="ICON[t.tone]" :size="18" />
          <span class="toast-text">{{ t.text }}</span>
          <button v-if="t.action" class="toast-action" @click="t.action.run(); toast.dismiss(t.id)">
            {{ t.action.label }}
          </button>
          <button class="toast-close" aria-label="关闭" @click="toast.dismiss(t.id)">
            <UiIcon name="close" :size="16" />
          </button>
        </div>
      </TransitionGroup>
    </div>
  </Teleport>
</template>

<style scoped>
.toast-host {
  position: fixed;
  left: 50%;
  transform: translateX(-50%);
  z-index: 9200;
  display: flex;
  flex-direction: column;
  gap: 8px;
  width: min(520px, calc(100vw - 32px));
  pointer-events: none;
}
.toast {
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 14px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-highest);
  color: var(--md-on-surface);
  box-shadow: var(--md-elev-2);
  border-left: 3px solid var(--md-outline);
  font-size: 14px;
}
.toast.ok { border-left-color: var(--md-success); }
.toast.ok :deep(.ui-icon) { color: var(--md-success); }
.toast.warn { border-left-color: var(--md-warning); }
.toast.warn :deep(.ui-icon) { color: var(--md-warning); }
.toast.error { border-left-color: var(--md-error); }
.toast.error :deep(.ui-icon) { color: var(--md-error); }
.toast-text { flex: 1; line-height: 1.45; }
.toast-action {
  border: none;
  background: transparent;
  color: var(--md-primary);
  font: 600 13px var(--font-body);
  cursor: pointer;
}
.toast-close {
  border: none;
  background: transparent;
  color: var(--md-on-surface-variant);
  cursor: pointer;
  display: grid;
  place-items: center;
  padding: 2px;
}
.toast-enter-active { transition: all var(--dur) var(--ease-spring); }
.toast-leave-active { transition: all var(--dur-fast) ease; position: absolute; width: 100%; }
.toast-enter-from { opacity: 0; transform: translateY(16px) scale(0.96); }
.toast-leave-to { opacity: 0; transform: translateY(8px); }
</style>
