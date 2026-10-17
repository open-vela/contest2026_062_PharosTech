<script setup lang="ts">
/* Ctrl/Cmd+K palette: navigate pages, switch expressions/scenes, run plugin
 * quick actions (v1.1 surface). Extensible via the host registry. */
import { computed, nextTick, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { MODES, SCENES } from '@nyabula/eye-engine';
import { UiIcon } from '@nyabula/ui';
import { PRIMARY_NAV } from '../nav';
import { useSessionStore } from '../stores/session';
import { MODE_LABELS, SCENE_META, useEyeStore } from '../stores/eye';
import { usePluginsStore } from '../stores/plugins';
import { hostRegistry } from '../host/registry';

const props = defineProps<{ modelValue: boolean }>();
const emit = defineEmits<{ (e: 'update:modelValue', v: boolean): void }>();
const router = useRouter();
const session = useSessionStore();
const eye = useEyeStore();
const plugins = usePluginsStore();
const q = ref('');
const cursor = ref(0);
const input = ref<HTMLInputElement | null>(null);

interface Cmd { id: string; group: string; label: string; icon: string; hint?: string; run: () => void }

const commands = computed<Cmd[]>(() => {
  const key = session.deviceKey ?? session.lastDeviceKey;
  const out: Cmd[] = [];
  for (const n of PRIMARY_NAV) {
    out.push({ id: `nav:${n.id}`, group: '页面', label: n.label, icon: n.icon, run: () => (n.to.startsWith('/') ? router.push(n.to) : key && router.push({ name: n.to, params: { key } })) });
  }
  out.push({ id: 'nav:settings', group: '页面', label: '客户端设置', icon: 'computer', run: () => router.push('/settings') });
  out.push({ id: 'nav:connect', group: '页面', label: '连接设备', icon: 'link', run: () => router.push({ name: 'connect', query: { stay: '1' } }) });
  if (session.connected) {
    for (const m of MODES) out.push({ id: `mode:${m}`, group: '表情', label: MODE_LABELS[m] ?? m, icon: 'face', hint: m, run: () => eye.setMode(m) });
    for (const s of SCENES) out.push({ id: `scene:${s}`, group: '场景', label: SCENE_META[s]?.label ?? s, icon: SCENE_META[s]?.icon ?? 'widgets', hint: s, run: () => eye.setScene(s) });
    out.push({ id: 'scene:none', group: '场景', label: '退出场景', icon: 'close', run: () => eye.setScene(null) });
    for (const p of plugins.list) {
      out.push({ id: `plugin:${p.id}`, group: '插件', label: p.name, icon: p.icon ?? 'extension', hint: p.id, run: () => key && router.push({ name: 'plugin', params: { key, id: p.id } }) });
      for (const a of p.actions ?? []) {
        out.push({ id: `action:${p.id}:${a.id}`, group: '快捷操作', label: `${p.name} · ${a.label}`, icon: a.icon ?? 'bolt', run: () => plugins.sendEvent(p.id, { componentId: a.id, event: 'tap', command: a.command }) });
      }
    }
  }
  for (const c of hostRegistry.commands()) out.push({ id: c.id, group: c.group ?? '扩展', label: c.label, icon: c.icon ?? 'bolt', run: c.run });
  return out;
});

const filtered = computed(() => {
  const s = q.value.trim().toLowerCase();
  if (!s) return commands.value.slice(0, 40);
  return commands.value.filter((c) => c.label.toLowerCase().includes(s) || c.hint?.toLowerCase().includes(s) || c.group.includes(s)).slice(0, 40);
});

watch(() => props.modelValue, (v) => {
  if (v) {
    q.value = '';
    cursor.value = 0;
    void nextTick(() => input.value?.focus());
  }
});
watch(filtered, () => (cursor.value = 0));

function run(c: Cmd) {
  emit('update:modelValue', false);
  void c.run();
}
function onKey(e: KeyboardEvent) {
  if (e.key === 'ArrowDown') { e.preventDefault(); cursor.value = Math.min(filtered.value.length - 1, cursor.value + 1); }
  if (e.key === 'ArrowUp') { e.preventDefault(); cursor.value = Math.max(0, cursor.value - 1); }
  if (e.key === 'Enter' && filtered.value[cursor.value]) run(filtered.value[cursor.value]!);
}
</script>

<template>
  <Teleport to="body">
    <Transition name="fade">
      <div v-if="modelValue" class="cp-root" @click.self="emit('update:modelValue', false)">
        <div class="cp" role="dialog" aria-label="命令面板">
          <div class="cp-input">
            <UiIcon name="search" :size="20" />
            <input ref="input" v-model="q" placeholder="搜索页面、表情、场景、插件…" @keydown="onKey" />
            <kbd>Esc</kbd>
          </div>
          <div class="cp-list">
            <button
              v-for="(c, i) in filtered"
              :key="c.id"
              class="cp-item"
              :class="{ on: i === cursor }"
              @mouseenter="cursor = i"
              @click="run(c)"
            >
              <UiIcon :name="c.icon" :size="18" />
              <span class="cp-label">{{ c.label }}</span>
              <span v-if="c.hint" class="cp-hint mono">{{ c.hint }}</span>
              <span class="cp-group">{{ c.group }}</span>
            </button>
            <p v-if="!filtered.length" class="cp-empty">没有匹配的命令</p>
          </div>
        </div>
      </div>
    </Transition>
  </Teleport>
</template>

<style scoped>
.cp-root { position: fixed; inset: 0; z-index: 8900; background: var(--md-scrim); display: flex; justify-content: center; padding-top: 12vh; }
.cp {
  width: min(640px, calc(100vw - 32px));
  height: fit-content;
  max-height: 70vh;
  background: var(--md-surface-container-high);
  border-radius: var(--radius-l);
  box-shadow: var(--md-elev-2), 0 30px 80px -20px rgba(0, 0, 0, 0.6);
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
.cp-input { display: flex; align-items: center; gap: 10px; padding: 14px 16px; border-bottom: 1px solid var(--md-outline-variant); color: var(--md-on-surface-variant); }
.cp-input input { flex: 1; background: transparent; border: none; outline: none; color: var(--md-on-surface); font: 500 16px var(--font-body); }
.cp-input kbd { font: 600 11px ui-monospace, monospace; padding: 2px 6px; border-radius: 6px; background: var(--md-surface-container-highest); }
.cp-list { overflow-y: auto; padding: 6px; }
.cp-item {
  width: 100%;
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 9px 12px;
  border: none;
  background: transparent;
  color: var(--md-on-surface);
  border-radius: var(--radius-s);
  cursor: pointer;
  text-align: left;
  font: 500 14px var(--font-body);
}
.cp-item.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.cp-label { flex: 1; }
.cp-hint { font-size: 12px; opacity: 0.6; }
.cp-group { font-size: 11px; padding: 2px 8px; border-radius: 999px; background: var(--md-surface-container-highest); color: var(--md-on-surface-variant); }
.cp-empty { text-align: center; color: var(--md-on-surface-variant); padding: 24px; font-size: 14px; }
</style>
