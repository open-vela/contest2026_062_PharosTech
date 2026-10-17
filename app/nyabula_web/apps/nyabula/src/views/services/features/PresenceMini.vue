<script setup lang="ts">
/* Presence mini: one row of member avatars; tapping toggles home / away
 * (home highlighted). Members are stored in nyabula.feature.presence
 * (shared with PresenceFeature) and every change pushes {members}. While
 * active the live payload decides who is home. */
import { computed, ref } from 'vue';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor; active: boolean; payload: Record<string, unknown> | null }>();
const eye = useEyeStore();

interface Member { id: string; name: string; home: boolean; lastAt: number }
const now = Date.now();
const mem = useFeatureMemory<{ members: Member[]; auto: boolean }>(props.type, {
  members: [
    { id: 'm1', name: '妈妈', home: true, lastAt: now - 12 * 60000 },
    { id: 'm2', name: '爸爸', home: false, lastAt: now - 3 * 3600000 },
    { id: 'm3', name: '小雨', home: true, lastAt: now - 40 * 60000 },
  ],
  auto: false,
});
const members = ref<Member[]>(Array.isArray(mem.members) ? mem.members : []);
const initial = (n: string): string => (n.trim()[0] ?? '?').toUpperCase();

/* Live home set from payload.members[{name, home}] while active. */
const liveHome = computed<Map<string, boolean> | null>(() => {
  const list = props.active ? props.payload?.members : null;
  if (!Array.isArray(list)) return null;
  const m = new Map<string, boolean>();
  for (const it of list as Array<{ name?: unknown; home?: unknown }>) {
    if (typeof it?.name === 'string') m.set(it.name, it.home === true);
  }
  return m;
});
const isHome = (m: Member): boolean => liveHome.value?.get(m.name) ?? m.home;
const homeCount = computed(() => members.value.filter(isHome).length);

function toggle(m: Member): void {
  m.home = !isHome(m);
  m.lastAt = Date.now();
  saveFeatureMemory(props.type, { members: members.value, auto: mem.auto });
  void eye.setScene(props.type, eye.sceneStyle, { members: members.value.map((x) => ({ name: x.name, home: x.home })) });
}
</script>

<template>
  <div class="pr">
    <div class="avatars">
      <button
        v-for="m in members"
        :key="m.id"
        type="button"
        class="av"
        :class="{ home: isHome(m) }"
        :title="m.name + (isHome(m) ? ' · 在家' : ' · 离家')"
        :aria-label="m.name + (isHome(m) ? '，在家，点击设为离家' : '，离家，点击设为在家')"
        @click="toggle(m)"
      >
        <span class="ini">{{ initial(m.name) }}</span>
        <span class="nm">{{ m.name }}</span>
      </button>
      <span v-if="!members.length" class="empty">还没有家庭成员</span>
    </div>
    <span class="count">{{ members.length ? (homeCount ? `${homeCount} 人在家` : '家里没人') : '' }}</span>
  </div>
</template>

<style scoped>
.pr { display: flex; align-items: center; gap: 10px; }
.avatars { flex: 1; min-width: 0; display: flex; gap: 6px; overflow-x: auto; scrollbar-width: none; }
.avatars::-webkit-scrollbar { display: none; }
.av {
  display: flex; flex-direction: column; align-items: center; gap: 2px; flex: none;
  min-width: 48px; padding: 4px 4px 2px; border: none; border-radius: var(--radius-m); cursor: pointer;
  background: transparent; color: var(--md-on-surface-variant); font: inherit;
}
.ini {
  width: 36px; height: 36px; border-radius: 50%; display: grid; place-items: center;
  background: var(--md-surface-container-highest); color: var(--md-on-surface); font: 700 14px var(--font-body);
  transition: background var(--dur-fast), color var(--dur-fast);
}
.av.home .ini { background: var(--md-primary); color: var(--md-on-primary); }
.av.home { color: var(--md-primary); }
.nm { font-size: 11px; max-width: 48px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.empty { font-size: 13px; color: var(--md-on-surface-variant); }
.count { font-size: 12px; color: var(--md-on-surface-variant); flex: none; white-space: nowrap; }
</style>
