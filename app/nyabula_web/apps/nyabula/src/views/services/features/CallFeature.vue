<script setup lang="ts">
/* Call feature. Local contact list (4 samples, add more) plus a call state
 * card (idle / incoming / active) with a big initial avatar and a duration
 * counter, and answer / hang-up / mute controls. Call state is mirrored to
 * the device scene; hanging up clears it. Contacts are persisted locally. */
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { BottomSheet, MdButton, MdTextField, NkActionBar, NkContactList, NkListSection, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import type { NkContact } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();

interface Contact { id: string; name: string; note: string }
const mem = useFeatureMemory<{ contacts: Contact[] }>(props.type, {
  contacts: [
    { id: 'c1', name: '妈妈', note: '家人' },
    { id: 'c2', name: '爸爸', note: '家人' },
    { id: 'c3', name: '小雨', note: '朋友' },
    { id: 'c4', name: '王医生', note: '宠物医院' },
  ],
});
const contacts = ref<Contact[]>(mem.contacts);
watch(contacts, () => saveFeatureMemory(props.type, { contacts: contacts.value }), { deep: true });

type CallState = 'idle' | 'incoming' | 'active';
const state = ref<CallState>('idle');
const peer = ref<Contact | null>(null);
const muted = ref(false);
const durationMs = ref(0);
let startAt = 0;
let timer: ReturnType<typeof setInterval> | undefined;
let lastPush = 0;

const initial = (n: string): string => (n.trim()[0] ?? '?').toUpperCase();
const pad = (n: number): string => String(n).padStart(2, '0');
const durationText = computed(() => {
  const s = Math.floor(durationMs.value / 1000);
  return `${pad(Math.floor(s / 60))}:${pad(s % 60)}`;
});
const stateText = computed(() => (state.value === 'incoming' ? '来电' : state.value === 'active' ? (muted.value ? '通话中 · 已静音' : '通话中') : '空闲'));
const subtitle = computed(() => (state.value === 'idle' ? '点联系人发起通话' : `${peer.value?.name ?? ''} · ${stateText.value}${state.value === 'active' ? ' ' + durationText.value : ''}`));

const list = computed<NkContact[]>(() =>
  contacts.value.map((c) => ({ id: c.id, name: c.name, status: c.note, actionIcon: 'call', online: peer.value?.id === c.id && state.value !== 'idle' })),
);

function push(sceneState: 'incoming' | 'active' | 'ended'): void {
  lastPush = Date.now();
  void eye.setScene(props.type, eye.sceneStyle, {
    name: peer.value?.name ?? '',
    state: sceneState,
    duration_ms: durationMs.value,
    muted: muted.value,
  });
}
function stopTick(): void {
  if (timer) clearInterval(timer);
  timer = undefined;
}
function tick(): void {
  durationMs.value = Date.now() - startAt;
  if (Date.now() - lastPush >= 5000) push('active');
}
/* Simulate an incoming call from a contact (no device call stack yet). */
function ring(c: Contact): void {
  if (state.value !== 'idle') return;
  peer.value = c;
  muted.value = false;
  durationMs.value = 0;
  state.value = 'incoming';
  push('incoming');
}
function answer(): void {
  if (state.value !== 'incoming') return;
  state.value = 'active';
  startAt = Date.now();
  stopTick();
  timer = setInterval(tick, 1000);
  push('active');
}
function hangUp(): void {
  stopTick();
  if (state.value !== 'idle') push('ended');
  state.value = 'idle';
  peer.value = null;
  muted.value = false;
  durationMs.value = 0;
  void eye.setScene(null);
}
function toggleMute(): void {
  muted.value = !muted.value;
  if (state.value === 'active') push('active');
}
onBeforeUnmount(stopTick);

/* Add contact. */
const adding = ref(false);
const nName = ref('');
const nNote = ref('');
const sheetMode = computed(() => props.ff !== 'desktop');
function openAdd(): void { nName.value = ''; nNote.value = ''; adding.value = true; }
function saveContact(): void {
  const n = nName.value.trim();
  if (!n) return;
  contacts.value.push({ id: `c${Date.now()}`, name: n, note: nNote.value.trim() });
  adding.value = false;
}
function removeContact(id: string): void {
  contacts.value = contacts.value.filter((c) => c.id !== id);
}
const removing = ref(false);
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="call" title="通话" :subtitle="subtitle" :tone="state === 'incoming' ? 'warn' : state === 'active' ? 'ok' : 'default'">
      <MdButton variant="tonal" @click="openAdd"><UiIcon name="add" :size="18" /> 添加</MdButton>
    </NkHeader>
    <div class="grid">
      <section class="card call-card" :class="state">
        <div class="avatar" :class="{ ringing: state === 'incoming' }">{{ peer ? initial(peer.name) : '' }}
          <UiIcon v-if="!peer" name="call" :size="40" />
        </div>
        <div class="who">
          <span class="name">{{ peer?.name ?? '没有进行中的通话' }}</span>
          <span class="muted">{{ state === 'active' ? durationText : stateText }}</span>
        </div>
        <div v-if="state === 'active'" class="ctrl-row">
          <button class="ctrl" :class="{ on: muted }" type="button" @click="toggleMute">
            <UiIcon :name="muted ? 'volume_off' : 'mic'" :size="24" />
            <span>{{ muted ? '取消静音' : '静音' }}</span>
          </button>
        </div>
        <NkActionBar
          v-if="state === 'incoming'"
          primary-text="接听" primary-icon="call"
          secondary-text="拒绝" secondary-icon="close"
          @primary="answer" @secondary="hangUp"
        />
        <NkActionBar v-else-if="state === 'active'" primary-text="挂断" primary-icon="close" danger @primary="hangUp" />
        <p v-else class="muted hint">从右侧联系人选择一位，模拟来电（设备通话能力接入后自动切换）<span class="contract-only">契约预留</span></p>
      </section>

      <section class="card">
        <NkListSection title="联系人" :card="false">
          <NkContactList :contacts="list" empty-text="还没有联系人" @tap="(c) => ring(contacts.find((x) => x.id === c.id)!)" @action="(c) => removing ? removeContact(c.id) : ring(contacts.find((x) => x.id === c.id)!)" />
          <template #trailing>
            <MdButton variant="text" @click="removing = !removing">{{ removing ? '完成' : '管理' }}</MdButton>
          </template>
        </NkListSection>
        <p v-if="removing" class="muted small">管理模式：点联系人右侧按钮删除</p>
      </section>
    </div>

    <template v-if="!sheetMode">
      <section v-if="adding" class="card">
        <h3 class="section-title">添加联系人</h3>
        <div class="row2">
          <MdTextField v-model="nName" label="姓名" placeholder="如：奶奶" />
          <MdTextField v-model="nNote" label="备注" placeholder="如：家人" />
        </div>
        <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!nName.trim()" @primary="saveContact" @secondary="adding = false" />
      </section>
    </template>
    <BottomSheet v-else :open="adding" title="添加联系人" @close="adding = false">
      <div class="sheet-body">
        <MdTextField v-model="nName" label="姓名" placeholder="如：奶奶" />
        <MdTextField v-model="nNote" label="备注" placeholder="如：家人" />
        <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!nName.trim()" @primary="saveContact" @secondary="adding = false" />
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.call-card { align-items: center; justify-content: center; text-align: center; min-height: 320px; }
.call-card.incoming { background: var(--md-tertiary-container, var(--md-surface-container-high)); }
.call-card.active { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.avatar {
  display: flex; align-items: center; justify-content: center;
  width: 112px; height: 112px; border-radius: 50%;
  background: var(--md-primary); color: var(--md-on-primary);
  font-size: 48px; font-weight: 700;
}
.avatar.ringing { animation: ring 1.2s var(--ease-standard, ease) infinite; }
@keyframes ring {
  0%, 100% { box-shadow: 0 0 0 0 color-mix(in srgb, var(--md-primary) 45%, transparent); }
  50% { box-shadow: 0 0 0 18px transparent; }
}
.who { display: flex; flex-direction: column; align-items: center; gap: 4px; }
.name { font-size: 26px; font-weight: 700; }
.ctrl-row { display: flex; gap: 16px; }
.ctrl {
  display: flex; flex-direction: column; align-items: center; gap: 4px;
  min-width: 72px; min-height: 64px; padding: 8px; border: 0; border-radius: var(--radius-m);
  background: var(--md-surface-container-highest); color: var(--md-on-surface); font: inherit; font-size: 12px; cursor: pointer;
}
.ctrl.on { background: var(--md-on-surface); color: var(--md-surface); }
.hint { font-size: 13px; max-width: 320px; }
.small { font-size: 12px; margin: 0; }
.row2 { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.sheet-body { display: flex; flex-direction: column; gap: 16px; padding: 4px 0 8px; }
</style>
