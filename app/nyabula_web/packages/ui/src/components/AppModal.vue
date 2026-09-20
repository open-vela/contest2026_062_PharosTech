<script setup lang="ts">
/* Single global modal host driven by the dialog store. Renders centered on
 * desktop/tablet, as a bottom sheet when `sheetMode` is set (phone). */
import { nextTick, ref, watch } from 'vue';
import { useDialogStore } from '../stores/dialog';
import MdButton from './MdButton.vue';

const dialog = useDialogStore();
const inputValue = ref('');
const inputRef = ref<HTMLInputElement | null>(null);

watch(
  () => dialog.active?.id,
  () => {
    inputValue.value = dialog.active?.input?.value ?? '';
    if (dialog.active?.kind === 'prompt') void nextTick(() => inputRef.value?.focus());
  },
);

function cancel() {
  const d = dialog.active;
  if (!d) return;
  dialog.settle(d.kind === 'confirm' ? false : d.kind === 'prompt' ? null : undefined);
}
function ok() {
  const d = dialog.active;
  if (!d) return;
  dialog.settle(d.kind === 'confirm' ? true : d.kind === 'prompt' ? inputValue.value : undefined);
}
function onKey(e: KeyboardEvent) {
  if (e.key === 'Escape') cancel();
  if (e.key === 'Enter' && dialog.active?.kind !== 'prompt') ok();
}
</script>

<template>
  <Teleport to="body">
    <Transition name="modal">
      <div
        v-if="dialog.active"
        class="modal-root"
        :class="{ sheet: dialog.sheetMode }"
        role="dialog"
        aria-modal="true"
        tabindex="-1"
        @keydown="onKey"
        @click.self="cancel"
      >
        <div class="modal-card">
          <h3 v-if="dialog.active.title" class="modal-title">{{ dialog.active.title }}</h3>
          <p v-if="dialog.active.message" class="modal-msg">{{ dialog.active.message }}</p>
          <label v-if="dialog.active.kind === 'prompt'" class="modal-input">
            <span v-if="dialog.active.input?.label">{{ dialog.active.input.label }}</span>
            <input
              ref="inputRef"
              v-model="inputValue"
              type="text"
              :placeholder="dialog.active.input?.placeholder"
              @keyup.enter="ok"
            />
          </label>
          <div class="modal-actions">
            <MdButton v-if="dialog.active.kind !== 'alert'" variant="text" @click="cancel">
              {{ dialog.active.cancelText ?? '取消' }}
            </MdButton>
            <MdButton :variant="dialog.active.danger ? 'filled' : 'filled'" :class="{ danger: dialog.active.danger }" @click="ok">
              {{ dialog.active.confirmText ?? '确定' }}
            </MdButton>
          </div>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.modal-root {
  position: fixed;
  inset: 0;
  z-index: 9100;
  background: var(--md-scrim);
  backdrop-filter: blur(6px);
  -webkit-backdrop-filter: blur(6px);
  display: grid;
  place-items: center;
  padding: 24px;
}
.modal-card {
  width: min(440px, 100%);
  background: var(--md-surface-container-high);
  border-radius: var(--radius-l);
  padding: 24px 24px 18px;
  box-shadow: var(--md-elev-2), 0 24px 64px -20px rgba(0, 0, 0, 0.5);
  transition: transform var(--dur) var(--ease-spring), opacity var(--dur) ease;
}
.modal-title {
  font: 600 18px var(--font-title);
  color: var(--md-on-surface);
  margin: 0 0 10px;
}
.modal-msg {
  color: var(--md-on-surface-variant);
  font-size: 14.5px;
  line-height: 1.6;
  margin: 0;
  white-space: pre-wrap;
}
.modal-input {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-top: 14px;
  font-size: 13px;
  color: var(--md-on-surface-variant);
}
.modal-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 20px;
}
.danger {
  background: var(--md-error) !important;
  color: var(--md-on-error) !important;
}
/* phone: bottom sheet */
.modal-root.sheet {
  place-items: end center;
  padding: 0;
}
.modal-root.sheet .modal-card {
  width: 100%;
  border-radius: var(--radius-l) var(--radius-l) 0 0;
  padding-bottom: calc(18px + env(safe-area-inset-bottom));
}
.modal-enter-active,
.modal-leave-active {
  transition: opacity var(--dur-fast) ease;
}
.modal-enter-from,
.modal-leave-to {
  opacity: 0;
}
.modal-enter-from .modal-card {
  transform: translateY(16px) scale(0.96);
}
.modal-root.sheet.modal-enter-from .modal-card {
  transform: translateY(100%);
}
</style>
