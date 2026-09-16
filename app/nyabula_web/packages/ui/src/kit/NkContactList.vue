<script setup lang="ts">
/* Contact list: initial avatar (or image), name, status line and a trailing
 * action button per row. Emits `tap` and `action` with the contact. */
import UiIcon from '../components/UiIcon.vue';

export interface NkContact {
  id: string;
  name: string;
  status?: string;
  avatar?: string;
  online?: boolean;
  /** Icon of the trailing action button (e.g. "call"); omit to hide. */
  actionIcon?: string;
}
export interface NkContactListProps {
  contacts: NkContact[];
  emptyText?: string;
}
withDefaults(defineProps<NkContactListProps>(), { emptyText: '暂无联系人' });
const emit = defineEmits<{ (e: 'tap', c: NkContact): void; (e: 'action', c: NkContact): void }>();

const initial = (name: string): string => (name || '?').trim().charAt(0).toUpperCase();
/* Deterministic hue per id so avatars differ without hard-coded colours. */
const hue = (id: string): number => {
  let h = 0;
  for (const ch of id) h = (h * 31 + ch.charCodeAt(0)) % 360;
  return h;
};
</script>

<template>
  <ul class="nk-contacts" role="list">
    <li v-if="!contacts.length" class="nk-contact-empty">{{ emptyText }}</li>
    <li
      v-for="c in contacts"
      :key="c.id"
      class="nk-contact"
      role="button"
      tabindex="0"
      @click="emit('tap', c)"
      @keydown.enter.prevent="emit('tap', c)"
    >
      <span class="nk-contact-avatar" :style="{ '--h': hue(c.id) }">
        <img v-if="c.avatar" :src="c.avatar" alt="" />
        <template v-else>{{ initial(c.name) }}</template>
        <span v-if="c.online !== undefined" class="nk-contact-dot" :class="{ on: c.online }" />
      </span>
      <span class="nk-contact-main">
        <span class="nk-contact-name">{{ c.name }}</span>
        <span v-if="c.status" class="nk-contact-status">{{ c.status }}</span>
      </span>
      <button
        v-if="c.actionIcon"
        type="button"
        class="nk-contact-action"
        :aria-label="c.actionIcon"
        @click.stop="emit('action', c)"
      >
        <UiIcon :name="c.actionIcon" :size="20" />
      </button>
    </li>
  </ul>
</template>

<style scoped>
.nk-contacts { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
.nk-contact-empty { padding: 16px; text-align: center; font: 500 13px var(--font-body); color: var(--md-on-surface-variant); }
.nk-contact {
  display: flex; align-items: center; gap: 12px;
  min-height: 56px; padding: 8px 12px;
  border-radius: var(--radius-m);
  color: var(--md-on-surface); cursor: pointer;
  transition: background var(--dur-fast);
}
.nk-contact:hover, .nk-contact:focus-visible { background: var(--md-surface-container-high); outline: none; }
.nk-contact-avatar {
  position: relative;
  width: 40px; height: 40px; flex: none;
  border-radius: var(--radius-full);
  display: grid; place-items: center;
  background: hsl(var(--h) 45% 45%);
  color: #fff;
  font: 700 16px var(--font-title);
  overflow: visible;
}
.nk-contact-avatar img { width: 100%; height: 100%; border-radius: inherit; object-fit: cover; }
.nk-contact-dot {
  position: absolute; right: -1px; bottom: -1px;
  width: 12px; height: 12px; border-radius: 50%;
  background: var(--md-outline);
  border: 2px solid var(--md-surface-container);
}
.nk-contact-dot.on { background: var(--md-success); }
.nk-contact-main { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
.nk-contact-name { font: 600 14px var(--font-body); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.nk-contact-status { font: 400 12px var(--font-body); color: var(--md-on-surface-variant); }
.nk-contact-action {
  width: 44px; height: 44px; flex: none;
  border: none; border-radius: var(--radius-full);
  background: var(--md-primary-container); color: var(--md-on-primary-container);
  display: grid; place-items: center; cursor: pointer;
  transition: transform var(--dur-fast);
}
.nk-contact-action:active { transform: scale(0.94); }
.nk-contact-action:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 2px; }
</style>
