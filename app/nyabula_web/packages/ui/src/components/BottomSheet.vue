<script setup lang="ts">
/* Bottom sheet (phone/tablet secondary surface). Drag the handle down to
 * dismiss. `side` renders as a right-side drawer instead (tablet/desktop). */
import { ref } from 'vue';
import UiIcon from './UiIcon.vue';

defineProps<{ open: boolean; title?: string; side?: boolean; maxHeight?: string }>();
const emit = defineEmits<{ (e: 'close'): void }>();

const dragY = ref(0);
let startY = 0;
let active = false;
function onDown(e: PointerEvent) {
  startY = e.clientY;
  active = true;
  (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
}
function onMove(e: PointerEvent) {
  if (!active) return;
  dragY.value = Math.max(0, e.clientY - startY);
}
function onUp() {
  if (!active) return;
  active = false;
  if (dragY.value > 90) emit('close');
  dragY.value = 0;
}
</script>

<template>
  <Teleport to="body">
    <Transition :name="side ? 'drawer' : 'sheet'">
      <div v-if="open" class="sheet-root" :class="{ side }" @click.self="emit('close')">
        <section
          class="sheet"
          :style="{ transform: dragY ? `translateY(${dragY}px)` : undefined, maxHeight: side ? undefined : maxHeight ?? '86vh' }"
        >
          <div v-if="!side" class="handle-row" @pointerdown="onDown" @pointermove="onMove" @pointerup="onUp" @pointercancel="onUp">
            <span class="handle" />
          </div>
          <header v-if="title || side" class="sheet-head">
            <h3 class="sheet-title">{{ title }}</h3>
            <button class="sheet-close" aria-label="关闭" @click="emit('close')"><UiIcon name="close" :size="20" /></button>
          </header>
          <div class="sheet-body"><slot /></div>
        </section>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.sheet-root {
  position: fixed;
  inset: 0;
  z-index: 8800;
  background: var(--md-scrim);
  display: flex;
  align-items: flex-end;
  justify-content: center;
}
.sheet-root.side { align-items: stretch; justify-content: flex-end; }
.sheet {
  width: 100%;
  max-width: 720px;
  background: var(--md-surface-container);
  border-radius: var(--radius-l) var(--radius-l) 0 0;
  box-shadow: 0 -8px 40px rgba(0, 0, 0, 0.35);
  display: flex;
  flex-direction: column;
  padding-bottom: env(safe-area-inset-bottom);
  transition: transform var(--dur) var(--ease-out);
}
.side .sheet {
  width: min(420px, 92vw);
  max-width: none;
  height: 100%;
  border-radius: var(--radius-l) 0 0 var(--radius-l);
}
.handle-row { display: grid; place-items: center; padding: 10px 0 4px; cursor: grab; touch-action: none; }
.handle { width: 40px; height: 4px; border-radius: 999px; background: var(--md-outline-variant); }
.sheet-head { display: flex; align-items: center; justify-content: space-between; padding: 8px 18px 4px; }
.sheet-title { margin: 0; font: 600 17px var(--font-title); color: var(--md-on-surface); }
.sheet-close { border: none; background: transparent; color: var(--md-on-surface-variant); cursor: pointer; display: grid; place-items: center; }
.sheet-body { overflow-y: auto; padding: 8px 18px 18px; }
.sheet-enter-active, .sheet-leave-active, .drawer-enter-active, .drawer-leave-active { transition: opacity var(--dur) ease; }
.sheet-enter-active .sheet, .sheet-leave-active .sheet, .drawer-enter-active .sheet, .drawer-leave-active .sheet { transition: transform var(--dur) var(--ease-out); }
.sheet-enter-from, .sheet-leave-to, .drawer-enter-from, .drawer-leave-to { opacity: 0; }
.sheet-enter-from .sheet, .sheet-leave-to .sheet { transform: translateY(100%); }
.drawer-enter-from .sheet, .drawer-leave-to .sheet { transform: translateX(100%); }
</style>
