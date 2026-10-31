<script setup lang="ts">
/* Floating device pill (design-lab A): pulse dot + device name + live
 * connection line, dropdown with known devices and "add / manage". Same
 * behaviour as components/DeviceSwitcher.vue plus the StatusPill info. */
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { UiIcon } from '@nyabula/ui';
import { useShell } from '../../../composables/useShell';

defineProps<{ compact?: boolean }>();
const shell = useShell();
const session = shell.session;
const router = useRouter();
const open = ref(false);
const root = ref<HTMLElement | null>(null);

const current = computed(() => session.known.find((d) => d.key === session.deviceKey) ?? null);
const name = computed(() => session.device?.name ?? current.value?.label ?? '选择设备');
const sub = computed(() => {
  if (session.state === 'connected') return session.transport === 'cloud' ? '在线 · 云中继' : '在线 · 局域网';
  return shell.connLabel.value;
});

function pick(key: string) {
  open.value = false;
  void router.push({ name: 'home', params: { key } });
}
function toConnect() {
  open.value = false;
  void router.push({ name: 'connect', query: { stay: '1' } });
}
function onDoc(e: MouseEvent) {
  if (open.value && root.value && !root.value.contains(e.target as Node)) open.value = false;
}
onMounted(() => document.addEventListener('pointerdown', onDoc));
onBeforeUnmount(() => document.removeEventListener('pointerdown', onDoc));
</script>

<template>
  <div ref="root" class="nb-devpill" @keydown.escape="open = false">
    <button class="pill nb-glass" :class="{ compact }" :aria-expanded="open" @click="open = !open">
      <span class="dot" :class="shell.connTone.value" />
      <span class="text">
        <span class="name">{{ name }}</span>
        <span v-if="!compact" class="sub">{{ sub }}</span>
      </span>
      <UiIcon name="expand_more" :size="18" class="chev" :class="{ flip: open }" />
    </button>
    <Transition name="nb-pop">
      <div v-if="open" class="menu nb-glass">
        <p class="nb-kicker">我的设备</p>
        <button v-for="d in session.known" :key="d.key" class="row" :class="{ on: d.key === session.deviceKey }" @click="pick(d.key)">
          <UiIcon :name="d.transport === 'cloud' ? 'cloud' : 'wifi'" :size="17" class="row-ico" />
          <span class="row-body"><span class="row-name">{{ d.label }}</span><span class="row-sub">{{ d.address }}</span></span>
          <UiIcon v-if="d.key === session.deviceKey" name="check" :size="16" class="row-check" />
        </button>
        <p v-if="!session.known.length" class="empty">还没有连接过设备</p>
        <button class="row add" @click="toConnect"><UiIcon name="add" :size="17" /> 添加 / 管理设备</button>
      </div>
    </Transition>
  </div>
</template>

<style scoped>
.nb-devpill { position: relative; min-width: 0; }
.pill {
  display: flex;
  align-items: center;
  gap: 12px;
  max-width: 300px;
  padding: 8px 14px 8px 14px;
  border-radius: 999px;
  cursor: pointer;
  color: var(--nb-fg);
  font: inherit;
  transition: transform 0.35s var(--nb-spring);
}
.pill.compact { padding: 9px 12px; gap: 9px; max-width: 52vw; }
.pill:active { transform: scale(0.97); }
.dot { width: 9px; height: 9px; border-radius: 50%; flex: none; background: var(--nb-dim); }
.dot.ok { background: var(--nb-acc); animation: nb-pulse 2.4s ease-out infinite; }
.dot.busy { background: var(--nb-amber); animation: nb-blink 1.1s ease-in-out infinite; }
@keyframes nb-pulse { 0% { box-shadow: 0 0 0 0 rgba(var(--nb-acc-rgb), 0.55); } 70%, 100% { box-shadow: 0 0 0 9px rgba(var(--nb-acc-rgb), 0); } }
@keyframes nb-blink { 50% { opacity: 0.35; } }
.text { display: flex; flex-direction: column; align-items: flex-start; line-height: 1.25; min-width: 0; }
.name { font: 600 14.5px var(--font-body); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 100%; }
.sub { font-size: 11.5px; color: var(--nb-sub); white-space: nowrap; }
.chev { color: var(--nb-sub); transition: transform 0.4s var(--nb-spring); flex: none; }
.chev.flip { transform: rotate(180deg); }
.menu {
  position: absolute;
  z-index: 60;
  top: calc(100% + 8px);
  left: 0;
  width: 300px;
  max-width: calc(100vw - 28px);
  padding: 12px;
  background: var(--nb-glass-strong);
  display: grid;
  gap: 2px;
  transform-origin: top left;
}
.menu .nb-kicker { padding: 4px 8px 8px; }
.row {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px;
  border: none;
  background: transparent;
  border-radius: 14px;
  cursor: pointer;
  color: var(--nb-fg);
  text-align: left;
  font: inherit;
  transition: background 0.2s;
}
.row:hover { background: var(--nb-soft-2); }
.row.on { background: rgba(var(--nb-acc-rgb), 0.1); }
.row-ico { color: var(--nb-sub); }
.row.on .row-ico, .row-check { color: var(--nb-acc); }
.row-body { flex: 1; min-width: 0; display: flex; flex-direction: column; }
.row-name { font: 600 13.5px var(--font-body); }
.row-sub { font-size: 11.5px; color: var(--nb-sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.row.add { color: var(--nb-sub); border-top: 1px solid var(--nb-line); border-radius: 0 0 14px 14px; margin-top: 4px; padding-top: 12px; }
.row.add:hover { color: var(--nb-fg); }
.empty { margin: 0; padding: 6px 10px 10px; color: var(--nb-dim); font-size: 12.5px; }
</style>
