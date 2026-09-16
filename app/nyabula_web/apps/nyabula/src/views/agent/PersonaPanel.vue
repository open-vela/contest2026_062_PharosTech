<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';

interface Profile { revision: number; catName: string; ownerName: string; language: string; tone: string; instructions: string }
const session = useSessionStore();
const form = reactive<Profile>({ revision: 0, catName: 'Nyabula', ownerName: '', language: 'zh-CN', tone: 'warm', instructions: '' });
const saved = ref<Profile | null>(null);
const busy = ref(false);
const error = ref('');
const message = ref('');
const available = computed(() => session.connected && session.isOwner);
const dirty = computed(() => !!saved.value && (Object.keys(form) as (keyof Profile)[]).some(key => form[key] !== saved.value?.[key]));
const canSave = computed(() => available.value && !!saved.value && dirty.value && !busy.value &&
  !!form.catName.trim() && new TextEncoder().encode(form.instructions).length <= 2048);
async function request(save: boolean): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client;
  busy.value = true;
  try {
    const result = await session.request(save ? 'agent.profile.set' : 'agent.profile.get', save ? { ...form } : {});
    if (client !== session.client) return;
    const profile = result as unknown as Profile;
    saved.value = profile; Object.assign(form, profile); error.value = '';
    message.value = save ? '已保存，下次对话使用新资料。' : '资料由设备保存。';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
watch(() => [session.client, session.connected], () => { saved.value = null; message.value = ''; void request(false); }, { immediate: true });
</script>

<template>
  <section class="persona-panel">
    <h3>人格与主人资料</h3>
    <p>这些设置会进入 Nyabot 的下一次对话，包括自动化对话。独立外部 MCP 对话不使用主人资料。</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <form @submit.prevent="request(true)">
      <MdTextField v-model="form.catName" label="猫猫名称" :disabled="!available || busy" />
      <MdTextField v-model="form.ownerName" label="怎么称呼你" placeholder="留空则不指定" :disabled="!available || busy" />
      <label>回复语言<select v-model="form.language" aria-label="回复语言" :disabled="!available || busy">
        <option value="zh-CN">简体中文</option><option value="zh-TW">繁體中文</option>
        <option value="en">English</option><option value="auto">跟随对话语言</option>
      </select></label>
      <label>对话语气<select v-model="form.tone" aria-label="对话语气" :disabled="!available || busy">
        <option value="warm">温暖陪伴</option><option value="playful">活泼俏皮</option>
        <option value="concise">简洁直接</option><option value="calm">平静舒缓</option>
      </select></label>
      <label class="guidance">补充偏好<textarea v-model="form.instructions" aria-label="补充偏好" rows="4" :disabled="!available || busy" placeholder="例如：回答先讲结论；睡前讲故事时放慢节奏。" /></label>
      <div class="actions">
        <MdButton :disabled="!canSave">保存人格资料</MdButton>
        <MdButton type="button" variant="text" :disabled="!dirty || busy" @click="saved && Object.assign(form, saved)">撤销资料修改</MdButton>
        <MdButton type="button" variant="text" :disabled="!available || busy" @click="request(false)">重新读取资料</MdButton>
      </div>
      <p role="status">{{ dirty ? '资料尚未保存' : message }}</p>
    </form>
  </section>
</template>

<style scoped>
.persona-panel { padding-top: 20px; margin-top: 28px; border-top: 1px solid var(--md-outline-variant); }
form { display: flex; flex-direction: column; gap: 16px; }
label { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }
.guidance { align-items: stretch; flex-direction: column; }
select, textarea { min-height: 44px; padding: 10px; border: 1px solid var(--md-outline); border-radius: 8px; color: var(--md-on-surface); background: var(--md-surface); font: inherit; }
textarea { resize: vertical; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.error { color: var(--md-error); }
</style>
