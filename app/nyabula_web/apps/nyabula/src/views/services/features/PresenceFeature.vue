<script setup lang="ts">
/* Presence feature. Family member cards (initial avatar, home/away toggle,
 * last-activity time) in a grid, plus an auto-detect switch that is reserved
 * in the contract but not yet backed by the device. Members are persisted
 * locally and mirrored to the device scene on every change. */
import { computed, ref, watch } from 'vue';
import { BottomSheet, MdButton, MdSwitch, MdTextField, NkActionBar, NkListSection, NkToggleRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
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
const members = ref<Member[]>(mem.members);
const auto = ref(mem.auto);
watch([members, auto], () => {
  saveFeatureMemory(props.type, { members: members.value, auto: auto.value });
  push();
}, { deep: true });

const initial = (n: string): string => (n.trim()[0] ?? '?').toUpperCase();
function ago(t: number): string {
  const m = Math.max(0, Math.round((Date.now() - t) / 60000));
  if (m < 1) return '刚刚';
  if (m < 60) return `${m} 分钟前`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} 小时前`;
  return `${Math.floor(h / 24)} 天前`;
}
const homeCount = computed(() => members.value.filter((m) => m.home).length);
const subtitle = computed(() => (members.value.length ? (homeCount.value ? `${homeCount.value} 人在家` : '家里没人') : '添加家庭成员'));

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, { members: members.value.map((m) => ({ name: m.name, home: m.home })) });
}
function setHome(m: Member, v: boolean): void {
  m.home = v;
  m.lastAt = Date.now();
}
function remove(id: string): void {
  members.value = members.value.filter((m) => m.id !== id);
}

/* Add member. */
const adding = ref(false);
const nName = ref('');
const sheetMode = computed(() => props.ff !== 'desktop');
function openAdd(): void { nName.value = ''; adding.value = true; }
function save(): void {
  const n = nName.value.trim();
  if (!n) return;
  members.value.push({ id: `m${Date.now()}`, name: n, home: true, lastAt: Date.now() });
  adding.value = false;
}
const managing = ref(false);
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="presence" title="在场" :subtitle="subtitle" :tone="homeCount ? 'ok' : 'default'">
      <MdButton variant="text" @click="managing = !managing">{{ managing ? '完成' : '管理' }}</MdButton>
      <MdButton variant="tonal" @click="openAdd"><UiIcon name="add" :size="18" /> 添加</MdButton>
    </NkHeader>

    <section class="members">
      <p v-if="!members.length" class="muted empty">还没有家庭成员</p>
      <div v-for="m in members" :key="m.id" class="member" :class="{ home: m.home }">
        <div class="avatar">{{ initial(m.name) }}</div>
        <div class="info">
          <span class="name">{{ m.name }}</span>
          <span class="status">{{ m.home ? '在家' : '离家' }} · {{ ago(m.lastAt) }}</span>
        </div>
        <MdButton v-if="managing" variant="icon" @click="remove(m.id)"><UiIcon name="delete" :size="20" /></MdButton>
        <span v-else class="switch-wrap"><MdSwitch :model-value="m.home" @update:model-value="setHome(m, $event)" /></span>
      </div>
    </section>

    <section class="card">
      <NkListSection title="自动检测" :card="false">
        <NkToggleRow v-model="auto" icon="visibility" title="自动识别在场" sub="通过摄像头与语音判断谁在家" />
        <template #trailing><span class="contract-only">契约预留</span></template>
      </NkListSection>
    </section>

    <section v-if="!sheetMode && adding" class="card">
      <h3 class="section-title">添加成员</h3>
      <MdTextField v-model="nName" label="姓名" placeholder="如：奶奶" />
      <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!nName.trim()" @primary="save" @secondary="adding = false" />
    </section>
    <BottomSheet v-if="sheetMode" :open="adding" title="添加成员" @close="adding = false">
      <div class="sheet-body">
        <MdTextField v-model="nName" label="姓名" placeholder="如：奶奶" />
        <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!nName.trim()" @primary="save" @secondary="adding = false" />
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.members { display: grid; grid-template-columns: 1fr; gap: 12px; }
.feature.tablet .members, .feature.desktop .members { grid-template-columns: repeat(2, 1fr); }
.empty { padding: 12px 0; text-align: center; }
.member {
  display: flex; align-items: center; gap: 14px; min-height: 76px; padding: 14px 16px;
  border-radius: var(--radius-l); background: var(--md-surface-container); color: var(--md-on-surface);
  transition: background var(--dur-short, 150ms) var(--ease-standard, ease);
}
.member.home { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.avatar {
  display: flex; align-items: center; justify-content: center; flex: none;
  width: 48px; height: 48px; border-radius: 50%;
  background: var(--md-surface-container-highest); color: var(--md-on-surface); font-size: 20px; font-weight: 700;
}
.member.home .avatar { background: var(--md-primary); color: var(--md-on-primary); }
.info { flex: 1; display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.name { font-size: 17px; font-weight: 600; }
.status { font-size: 13px; opacity: 0.8; }
.switch-wrap { display: inline-flex; align-items: center; min-height: 44px; }
.sheet-body { display: flex; flex-direction: column; gap: 16px; padding: 4px 0 8px; }
</style>
