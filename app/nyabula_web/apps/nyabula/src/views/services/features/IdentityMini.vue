<script setup lang="ts">
/* Identity mini: owner name + family avatars + recognition switch. Shares
 * nyabula.feature.identity with IdentityFeature; tapping the avatars pushes
 * { owner, members, recognizing } to the eye. */
import { computed, reactive, watch } from 'vue';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useFeatureMemory, saveFeatureMemory, type FeatureMiniProps } from './contract';

const props = defineProps<FeatureMiniProps>();
const eye = useEyeStore();

type Role = 'owner' | 'family' | 'guest';
interface Member { id: string; name: string; role: Role }
const state = reactive(useFeatureMemory(props.type, {
  members: [
    { id: 'u1', name: '小明', role: 'owner' },
    { id: 'u2', name: '妈妈', role: 'family' },
    { id: 'u3', name: '爸爸', role: 'family' },
  ] as Member[],
  recognition: true,
}));
watch(state, () => saveFeatureMemory(props.type, state), { deep: true });

const owner = computed(() => state.members.find((m) => m.role === 'owner') ?? null);
const others = computed(() => state.members.filter((m) => m.role !== 'owner').slice(0, 4));
const ownerName = computed(() => (props.active && typeof props.payload?.owner === 'string' && props.payload.owner ? props.payload.owner : owner.value?.name ?? '未设置'));
const recognizing = computed(() => props.active && props.payload?.recognizing === true);

function push(): void {
  void eye.setScene(props.type, eye.sceneStyle, {
    owner: owner.value?.name ?? '',
    members: state.members.map((m) => ({ name: m.name, role: m.role })),
    recognizing: false,
  });
}
function toggleScene(): void {
  if (props.active) void eye.setScene(null);
  else push();
}
function setRecognition(v: boolean): void {
  state.recognition = v;
}
</script>

<template>
  <div class="im">
    <button type="button" class="im-people" :title="active ? '隐藏' : '显示'" @click="toggleScene">
      <span class="im-owner">
        <UiIcon name="face" :size="16" />
        <span class="im-name">{{ ownerName }}</span>
        <span v-if="recognizing" class="im-tag">认主中</span>
      </span>
      <span class="im-avatars">
        <span v-for="m in others" :key="m.id" class="im-av" :title="m.name">{{ m.name.slice(0, 1) }}</span>
        <span v-if="!others.length" class="im-none">无家人</span>
      </span>
    </button>
    <label class="im-sw">
      <span>认主</span>
      <MdSwitch :model-value="state.recognition" @update:model-value="setRecognition" />
    </label>
  </div>
</template>

<style scoped>
.im { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.im-people { flex: 1; min-width: 0; display: flex; align-items: center; gap: 10px; border: none; background: transparent; padding: 0; cursor: pointer; text-align: left; font: inherit; color: var(--md-on-surface); min-height: 40px; }
.im-owner { display: inline-flex; align-items: center; gap: 5px; min-width: 0; }
.im-owner :deep(svg) { color: var(--md-primary); }
.im-name { font: 600 14px var(--font-body); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.im-tag { font: 600 10.5px var(--font-body); padding: 1px 6px; border-radius: 999px; background: var(--md-primary); color: var(--md-on-primary); flex: none; }
.im-avatars { display: inline-flex; margin-left: auto; flex: none; }
.im-av {
  width: 28px; height: 28px; border-radius: 50%; display: grid; place-items: center; font: 600 12px var(--font-body);
  background: var(--md-secondary-container); color: var(--md-on-secondary-container);
  border: 2px solid var(--md-surface-container); margin-left: -6px;
}
.im-av:first-child { margin-left: 0; }
.im-none { font-size: 12px; color: var(--md-on-surface-variant); }
.im-sw { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--md-on-surface-variant); min-height: 40px; flex: none; cursor: pointer; }
</style>
