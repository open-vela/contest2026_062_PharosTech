<script setup lang="ts">
/* Device switcher: dropdown (desktop/tablet) or inline list (phone "more"). */
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../stores/session';

const props = defineProps<{ compact?: boolean; list?: boolean }>();
const emit = defineEmits<{ (e: 'done'): void }>();
const session = useSessionStore();
const router = useRouter();
const open = ref(false);

const current = computed(() => session.known.find((d) => d.key === session.deviceKey) ?? null);
const label = computed(() => session.device?.name ?? current?.value?.label ?? '选择设备');

function pick(key: string) {
  open.value = false;
  emit('done');
  void router.push({ name: 'home', params: { key } });
}
function toConnect() {
  open.value = false;
  emit('done');
  void router.push({ name: 'connect', query: { stay: '1' } });
}
</script>

<template>
  <div v-if="list" class="dev-list">
    <p class="section-title">设备</p>
    <button v-for="d in session.known" :key="d.key" class="list-tile" @click="pick(d.key)">
      <span class="tile-icon"><UiIcon :name="d.transport === 'cloud' ? 'cloud' : 'wifi'" :size="20" /></span>
      <span class="tile-body"><span class="tile-title">{{ d.label }}</span><span class="tile-sub">{{ d.address }}</span></span>
      <span v-if="d.key === session.deviceKey" class="tile-trail"><UiIcon name="check" :size="20" /></span>
    </button>
    <button class="list-tile" @click="toConnect">
      <span class="tile-icon"><UiIcon name="add" :size="20" /></span>
      <span class="tile-body"><span class="tile-title">添加设备</span></span>
    </button>
  </div>
  <div v-else class="switcher" :class="{ compact: props.compact }" @keydown.escape="open = false">
    <button class="trigger" @click="open = !open">
      <UiIcon :name="session.transport === 'cloud' ? 'cloud' : 'wifi'" :size="18" />
      <span class="trigger-label">{{ label }}</span>
      <UiIcon name="expand_more" :size="18" />
    </button>
    <Transition name="pop">
      <div v-if="open" class="menu" @mouseleave="open = false">
        <button v-for="d in session.known" :key="d.key" class="menu-item" :class="{ on: d.key === session.deviceKey }" @click="pick(d.key)">
          <UiIcon :name="d.transport === 'cloud' ? 'cloud' : 'wifi'" :size="18" />
          <span class="mi-body"><span class="mi-title">{{ d.label }}</span><span class="mi-sub">{{ d.address }}</span></span>
          <UiIcon v-if="d.key === session.deviceKey" name="check" :size="18" />
        </button>
        <div v-if="session.known.length" class="menu-sep" />
        <button class="menu-item" @click="toConnect"><UiIcon name="add" :size="18" /><span class="mi-title">添加 / 管理设备</span></button>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.switcher { position: relative; }
.trigger {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  padding: 6px 10px 6px 12px;
  border-radius: 999px;
  border: 1px solid var(--md-outline-variant);
  background: var(--md-surface-container);
  color: var(--md-on-surface);
  font: 600 13px var(--font-body);
  cursor: pointer;
  max-width: 260px;
}
.compact .trigger-label { display: none; }
.trigger-label { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.menu {
  position: absolute;
  top: calc(100% + 6px);
  left: 0;
  min-width: 260px;
  background: var(--md-surface-container-high);
  border-radius: var(--radius-m);
  box-shadow: var(--md-elev-2);
  padding: 6px;
  z-index: 50;
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.compact .menu { left: auto; right: 0; }
.menu-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 8px 10px;
  border: none;
  background: transparent;
  color: var(--md-on-surface);
  border-radius: var(--radius-s);
  cursor: pointer;
  text-align: left;
  font: inherit;
}
.menu-item:hover, .menu-item.on { background: var(--md-surface-container-highest); }
.mi-body { flex: 1; display: flex; flex-direction: column; min-width: 0; }
.mi-title { font-weight: 600; font-size: 13.5px; }
.mi-sub { font-size: 12px; color: var(--md-on-surface-variant); }
.menu-sep { height: 1px; background: var(--md-outline-variant); margin: 4px 6px; }
.dev-list { display: flex; flex-direction: column; gap: 8px; }
</style>
