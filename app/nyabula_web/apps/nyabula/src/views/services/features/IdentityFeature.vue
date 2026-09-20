<script setup lang="ts">
/* Identity feature. Owner + family members (initial avatars, role tags) kept
 * locally; recognition toggle and "start recognizing" are reserved by
 * contract. Pushes { owner, members:[...], recognizing } to the eye. */
import { computed, reactive, ref, watch } from 'vue';
import { NkActionBar, NkContactList, NkListSection, NkToggleRow, MdButton, MdTextField, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import type { NkContact } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory } from './contract';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
const eye = useEyeStore();
const dialog = useDialogStore();
const toast = useToastStore();

type Role = 'owner' | 'family' | 'guest';
interface Member { id: string; name: string; role: Role }
const ROLE_LABEL: Record<Role, string> = { owner: '主人', family: '家人', guest: '访客' };

const mem = useFeatureMemory(props.type, {
  members: [
    { id: 'u1', name: '小明', role: 'owner' },
    { id: 'u2', name: '妈妈', role: 'family' },
    { id: 'u3', name: '爸爸', role: 'family' },
  ] as Member[],
  recognition: true,
});
const state = reactive(mem);
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const newName = ref('');
const recognizing = ref(false);
let recogTimer: ReturnType<typeof setTimeout> | undefined;

const owner = computed(() => state.members.find((m) => m.role === 'owner') ?? null);
const others = computed(() => state.members.filter((m) => m.role !== 'owner'));
const active = computed(() => eye.activeScene === props.type);
const subtitle = computed(() => (recognizing.value ? '正在认主…' : owner.value ? `主人 ${owner.value.name} · ${others.value.length} 位家人` : '还没有主人'));

const contacts = computed<NkContact[]>(() => state.members.map((m) => ({ id: m.id, name: m.name, status: ROLE_LABEL[m.role], actionIcon: 'delete' })));

function add(): void {
  const n = newName.value.trim();
  if (!n) return;
  state.members.push({ id: `${Date.now()}`, name: n, role: owner.value ? 'family' : 'owner' });
  newName.value = '';
}
async function remove(c: NkContact): Promise<void> {
  const m = state.members.find((x) => x.id === c.id);
  if (!m) return;
  const ok = await dialog.confirm(`移除 ${m.name}？设备将不再认识这个人。`, { title: '移除成员', danger: true, confirmText: '移除' });
  if (ok) state.members = state.members.filter((x) => x.id !== c.id);
}
function setOwner(c: NkContact): void {
  state.members.forEach((m) => { m.role = m.id === c.id ? 'owner' : m.role === 'owner' ? 'family' : m.role; });
}
function startRecognize(): void {
  if (recognizing.value) return;
  recognizing.value = true;
  push();
  recogTimer = setTimeout(() => {
    recognizing.value = false;
    toast.warn('认主流程为契约预留，设备暂未实现');
    push();
  }, 3000);
}
function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, {
    owner: owner.value?.name ?? '',
    members: state.members.map((m) => ({ name: m.name, role: m.role })),
    recognizing: recognizing.value,
  });
}
function hide(): void {
  if (recogTimer) clearTimeout(recogTimer);
  recognizing.value = false;
  void eye.setScene(null);
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="identity" title="身份" :subtitle="subtitle" :tone="recognizing || active ? 'ok' : 'default'" />
    <div class="grid">
      <section class="card">
        <div class="owner">
          <span class="avatar">{{ owner ? owner.name.charAt(0) : '?' }}</span>
          <div class="owner-text">
            <div class="owner-name">{{ owner?.name ?? '未设置主人' }}</div>
            <span class="tag ok">主人</span>
          </div>
        </div>
        <NkListSection>
          <NkToggleRow v-model="state.recognition" icon="face" title="认主" sub="见到主人时眼睛会变成爱心" />
        </NkListSection>
        <p class="muted small"><span class="contract-only">认主/人脸识别为契约预留</span>，当前为演示流程。</p>
        <NkActionBar
          :primary-text="recognizing ? '正在认主…' : '开始认主'"
          primary-icon="face"
          secondary-text="隐藏"
          secondary-icon="close"
          :busy="recognizing"
          :disabled="!state.recognition"
          @primary="startRecognize"
          @secondary="hide"
        />
        <MdButton variant="text" @click="push"><UiIcon name="visibility" :size="18" />显示成员到眼睛</MdButton>
      </section>
      <section class="card">
        <NkListSection title="家庭成员" :card="false">
          <template #trailing><span class="muted">{{ state.members.length }} 人</span></template>
          <NkContactList :contacts="contacts" empty-text="还没有成员" @tap="setOwner" @action="remove" />
        </NkListSection>
        <p class="muted small">点击成员可设为主人；点击右侧图标移除。</p>
        <div class="row add">
          <MdTextField v-model="newName" label="添加成员" placeholder="名字" @keydown.enter="add" />
          <MdButton variant="tonal" :disabled="!newName.trim()" @click="add"><UiIcon name="add" :size="20" />添加</MdButton>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid, .feature.tablet .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.owner { display: flex; align-items: center; gap: 14px; padding: 4px; }
.avatar {
  display: inline-flex; align-items: center; justify-content: center; width: 64px; height: 64px; flex: none;
  border-radius: 50%; font-size: 26px; font-weight: 700;
  background: var(--md-primary-container); color: var(--md-on-primary-container);
}
.owner-name { font-size: 20px; font-weight: 700; margin-bottom: 4px; }
.small { font-size: 12px; margin: 0; }
.add { align-items: flex-end; }
.add > :first-child { flex: 1; }
</style>
