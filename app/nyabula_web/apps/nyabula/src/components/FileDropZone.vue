<script setup lang="ts">
/* A file chooser in the panel's own clothes: a zone that takes a drop or a
 * click, and once a file is in, shows what it is (name, size, digest) with a
 * way to change or remove it.
 *
 * The native <input type="file"> is still what opens the system dialog - it is
 * the only thing that may - but it is never shown: its "选择文件 未选择文件"
 * text cannot be styled and follows the browser's language, not the panel's.
 * It stays in the tab order, so the zone works from the keyboard as well. */
import { computed, ref } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';

const props = defineProps<{
  file: File | null;
  /** Formatted size of `file`; the caller owns the formatter. */
  sizeText?: string;
  /** Full SHA-256 of `file`, '' until it is known. */
  digest?: string;
  /** Short form of `digest` for narrow places; the full one stays in the title. */
  digestShort?: string;
  /** Second line of the empty zone: what kind of file, how large. */
  hint?: string;
  accept?: string;
  disabled?: boolean;
  /** A file is in and being worked on: it can no longer be changed. */
  locked?: boolean;
}>();
const emit = defineEmits<{ (e: 'pick', file: File): void; (e: 'clear'): void }>();

const input = ref<HTMLInputElement | null>(null);
const over = ref(false);
/* dragenter / dragleave fire for every child the pointer crosses. */
let depth = 0;

const inert = computed(() => props.disabled === true || props.locked === true);

function browse(): void {
  if (!inert.value) input.value?.click();
}

function onChange(e: Event): void {
  const el = e.target as HTMLInputElement;
  const picked = el.files?.[0] ?? null;
  // Cleared so that picking the same file again is still a change.
  el.value = '';
  if (picked && !inert.value) emit('pick', picked);
}

function carriesFiles(e: DragEvent): boolean {
  return Array.from(e.dataTransfer?.types ?? []).includes('Files');
}

function onDragEnter(e: DragEvent): void {
  if (inert.value || !carriesFiles(e)) return;
  depth++;
  over.value = true;
}

function onDragOver(e: DragEvent): void {
  if (inert.value || !carriesFiles(e)) return;
  if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
}

function onDragLeave(): void {
  depth = Math.max(0, depth - 1);
  if (depth === 0) over.value = false;
}

function onDrop(e: DragEvent): void {
  depth = 0;
  over.value = false;
  if (inert.value) return;
  const dropped = e.dataTransfer?.files?.[0] ?? null;
  if (dropped) emit('pick', dropped);
}
</script>

<template>
  <div
    class="drop"
    :class="{ over, filled: !!file, inert }"
    @dragenter.prevent="onDragEnter"
    @dragover.prevent="onDragOver"
    @dragleave="onDragLeave"
    @drop.prevent="onDrop"
  >
    <input ref="input" class="native" type="file" :accept="accept" :disabled="inert" aria-label="选择文件" @change="onChange" />

    <button v-if="!file" type="button" class="empty" :disabled="inert" tabindex="-1" @click="browse()">
      <span class="orb"><UiIcon name="upload" :size="24" /></span>
      <span class="lead">{{ over ? '松开即可选中这个文件' : '把文件拖到这里，或点击选择' }}</span>
      <span v-if="hint" class="hint">{{ hint }}</span>
    </button>

    <div v-else class="chosen">
      <span class="orb small"><UiIcon name="article" :size="20" /></span>
      <div class="meta">
        <p class="name" :title="file.name">{{ file.name }}</p>
        <p class="sub">
          <span>{{ sizeText ?? `${file.size} 字节` }}</span>
          <span v-if="digest" class="mono sha" :title="`SHA-256 ${digest}`">SHA-256 {{ digestShort || digest }}</span>
          <span v-else class="sha pending">SHA-256 待计算</span>
        </p>
      </div>
      <div v-if="!locked" class="acts">
        <MdButton variant="text" :disabled="disabled" @click="browse()">更换</MdButton>
        <MdButton variant="text" :disabled="disabled" @click="emit('clear')"><UiIcon name="close" :size="16" /> 移除</MdButton>
      </div>
    </div>
  </div>
</template>

<style scoped>
.drop {
  position: relative;
  border: 1.5px dashed var(--md-outline);
  border-radius: var(--radius-m);
  background: var(--md-surface-container-high);
  transition: border-color var(--dur-fast), background var(--dur-fast);
}
.drop.filled { border-style: solid; border-color: var(--md-outline-variant); }
.drop.over { border-color: var(--md-primary); background: color-mix(in srgb, var(--md-primary) 12%, var(--md-surface-container-high)); }
.drop.inert { opacity: 0.6; }
/* Shown to nobody, reachable by everybody: focus lands here and the zone
 * draws the ring. */
.native { position: absolute; width: 1px; height: 1px; opacity: 0; overflow: hidden; clip: rect(0 0 0 0); }
.drop:has(.native:focus-visible) { outline: 2px solid var(--md-primary); outline-offset: 2px; }
.empty {
  width: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 26px 16px;
  border: 0;
  background: none;
  font: inherit;
  color: var(--md-on-surface);
  cursor: pointer;
  border-radius: inherit;
}
.empty:disabled { cursor: default; }
.empty:not(:disabled):hover .orb { background: color-mix(in srgb, var(--md-primary) 26%, transparent); }
.orb {
  display: grid;
  place-items: center;
  width: 48px;
  height: 48px;
  border-radius: 50%;
  color: var(--md-primary);
  background: color-mix(in srgb, var(--md-primary) 16%, transparent);
  transition: background var(--dur-fast);
  flex: none;
}
.orb.small { width: 40px; height: 40px; }
.lead { font-size: 14px; font-weight: 600; }
.hint { font-size: 12.5px; color: var(--md-on-surface-variant); text-align: center; line-height: 1.5; }
.chosen { display: flex; align-items: center; gap: 12px; padding: 12px 14px; flex-wrap: wrap; }
.meta { flex: 1 1 200px; min-width: 0; }
.name { margin: 0; font-size: 14px; font-weight: 600; color: var(--md-on-surface); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.sub { margin: 2px 0 0; display: flex; flex-wrap: wrap; gap: 4px 12px; font-size: 12.5px; color: var(--md-on-surface-variant); }
.sha { font-size: 12px; }
.sha.pending { opacity: 0.7; }
.acts { display: flex; gap: 2px; flex: none; margin-left: auto; }
.acts :deep(.ui-icon) { vertical-align: -3px; margin-right: 2px; }
</style>
