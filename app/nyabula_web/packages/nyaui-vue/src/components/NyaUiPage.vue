<script setup lang="ts">
/* NyaUiPage — root NyaUI renderer.
 * - Validates the tree against DSL limits; on violation shows an error card.
 * - Owns optimistic input state + per-component 300 ms change debounce.
 * - Emits `command` with {componentId, event, command, args, values} — the
 *   host turns that into a `ui.event` request. `props.confirm` on a node
 *   gates tap events behind a native confirm dialog. */
import { computed, onBeforeUnmount, provide, reactive, watch } from 'vue';
import { NYAUI_CTX } from '../context.js';
import { validateTree } from '../validate.js';
import { collectValues, resolveArgs } from '../values.js';
import { INPUT_TYPES, type NyaUiCommand, type NyaUiNode, type NyaUiTree } from '../types.js';
import NyaUiNodeComp from './NyaUiNode.vue';
import { registerKit } from '../kit.js';

/* Feature Kit types are available to every page by default. Idempotent. */
registerKit();

const props = defineProps<{ tree: NyaUiTree | null; busy?: boolean }>();
const emit = defineEmits<{ (e: 'command', payload: NyaUiCommand): void }>();

const CHANGE_DEBOUNCE_MS = 300;

const error = computed(() => validateTree(props.tree));

/** Optimistic local edits keyed by component id. Server is authoritative:
 * any tree change (full reload or ui.patch) resets the overlay. */
const localEdits = reactive<Record<string, unknown>>({});
const debounceTimers = new Map<string, ReturnType<typeof setTimeout>>();

watch(
  () => props.tree,
  () => {
    for (const k of Object.keys(localEdits)) delete localEdits[k];
  },
  { deep: true },
);

/** id -> node index for change-event lookup. */
const nodeById = computed(() => {
  const map = new Map<string, NyaUiNode>();
  const walk = (n: NyaUiNode | undefined): void => {
    if (!n) return;
    if (n.id) map.set(n.id, n);
    n.children?.forEach(walk);
  };
  if (!error.value) walk(props.tree?.root);
  return map;
});

function snapshot(): Record<string, unknown> {
  return collectValues(props.tree?.root, { ...localEdits });
}

function emitEvent(node: NyaUiNode, event: string, extra?: Record<string, unknown>): void {
  const action = node.on?.[event];
  const values = snapshot();
  const resolved = resolveArgs(action?.args, values);
  const args = extra ? { ...(resolved ?? {}), ...extra } : resolved;
  emit('command', {
    componentId: node.id,
    event,
    command: action?.command,
    args,
    values,
  });
}

function fire(node: NyaUiNode, event: string, extra?: Record<string, unknown>): void {
  const confirmText = node.props?.confirm;
  if (typeof confirmText === 'string' && confirmText.length > 0) {
    if (!window.confirm(confirmText)) return;
  }
  emitEvent(node, event, extra);
}

function getValue(id: string | undefined): unknown {
  if (!id) return undefined;
  if (id in localEdits) return localEdits[id];
  const node = nodeById.value.get(id);
  return node && INPUT_TYPES.has(node.type) ? node.props?.value : undefined;
}

function setValue(node: NyaUiNode, value: unknown, immediate: boolean): void {
  if (!node.id) return;
  localEdits[node.id] = value;
  const pending = debounceTimers.get(node.id);
  if (pending) clearTimeout(pending);
  if (immediate) {
    debounceTimers.delete(node.id);
    emitEvent(node, 'change');
  } else {
    debounceTimers.set(
      node.id,
      setTimeout(() => {
        debounceTimers.delete(node.id!);
        emitEvent(node, 'change');
      }, CHANGE_DEBOUNCE_MS),
    );
  }
}

onBeforeUnmount(() => {
  for (const t of debounceTimers.values()) clearTimeout(t);
  debounceTimers.clear();
});

provide(NYAUI_CTX, { getValue, setValue, fire });
</script>

<template>
  <div class="nyaui-page" :class="{ busy }">
    <div v-if="error" class="nyaui-error">
      <div class="nyaui-error-title">页面无法渲染</div>
      <div class="nyaui-error-msg">{{ error }}</div>
    </div>
    <NyaUiNodeComp v-else-if="tree" :node="tree.root" />
    <div v-if="busy" class="nyaui-busy-veil" />
  </div>
</template>

<style scoped>
.nyaui-page {
  position: relative;
}
.nyaui-page.busy {
  pointer-events: none;
}
.nyaui-busy-veil {
  position: absolute;
  inset: 0;
  background: var(--md-scrim, rgba(0, 0, 0, 0.25));
  opacity: 0.35;
  border-radius: var(--radius-m, 14px);
}
.nyaui-error {
  background: rgba(255, 180, 171, 0.12);
  border: 1px solid var(--md-error);
  border-radius: var(--radius-m, 14px);
  padding: 16px 18px;
  color: var(--md-error);
}
.nyaui-error-title {
  font: 600 15px var(--font-body, sans-serif);
  margin-bottom: 6px;
}
.nyaui-error-msg {
  font: 400 13px monospace;
  word-break: break-all;
}
</style>
