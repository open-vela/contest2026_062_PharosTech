<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import { MdButton, MdTextField, useDialogStore } from '@nyabula/ui';
import { useSessionStore } from '../../stores/session';

interface Skill { id: string; title: string; source: 'builtin' | 'custom'; enabled: boolean; content?: string }
const session = useSessionStore();
const dialog = useDialogStore();
const items = ref<Skill[]>([]);
const revision = ref(0);
const editRevision = ref(0);
const busy = ref(false);
const error = ref('');
const selected = ref<Skill | null>(null);
const editing = ref(false);
const form = reactive({ id: '', title: '', content: '' });
const available = computed(() => session.connected && session.isOwner);
const contentBytes = computed(() => new TextEncoder().encode(form.content).length);
const canSave = computed(() => available.value && !busy.value && /^[a-z0-9_-]{1,40}$/.test(form.id) &&
  form.title.trim().length > 0 && new TextEncoder().encode(form.title).length <= 96 && contentBytes.value > 0 && contentBytes.value <= 4096);
function apply(data: Record<string, unknown>): void {
  if (!Array.isArray(data.items) || typeof data.revision !== 'number') throw new Error('设备返回了无效的技能清单');
  items.value = data.items as Skill[]; revision.value = data.revision;
}
async function load(): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client; busy.value = true;
  try { const data = await session.request('agent.skills.list', {}); if (client === session.client) { apply(data); error.value = ''; } }
  catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
async function open(skill: Skill): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client; busy.value = true;
  try {
    const data = await session.request('agent.skills.get', { id: skill.id });
    if (client !== session.client) return;
    selected.value = data as unknown as Skill; editRevision.value = Number(data.revision);
    Object.assign(form, { id: data.id, title: data.title, content: data.content }); editing.value = true; error.value = '';
  } catch (cause) { if (client === session.client) error.value = String(cause); }
  finally { busy.value = false; }
}
async function change(topic: string, data: Record<string, unknown>): Promise<void> {
  if (!available.value || busy.value) return;
  const client = session.client; busy.value = true;
  try {
    const result = await session.request(topic, { ...data, revision: topic === 'agent.skills.save' ? editRevision.value : revision.value });
    if (client !== session.client) return;
    apply(result); error.value = ''; editing.value = false; selected.value = null;
  } catch (cause) {
    if (client === session.client) error.value = String(cause) + '。如记录已变化，请保留草稿，关闭编辑器后重新打开并核对。';
  } finally { busy.value = false; }
}
function create(): void { selected.value = null; editRevision.value = revision.value; Object.assign(form, { id: '', title: '', content: '' }); editing.value = true; }
async function toggle(skill: Skill): Promise<void> {
  if (!skill.enabled && !await dialog.confirm('启用后，Nyabot 可在后续请求中读取技能正文。技能不会授予新工具或绕过审批；内置技能需要的部分工具可能尚未接入。', { title: '启用技能', confirmText: '启用' })) return;
  await change('agent.skills.enable', { id: skill.id, enabled: !skill.enabled });
}
async function remove(skill: Skill): Promise<void> {
  if (await dialog.confirm('删除此自定义技能，不会删除已有聊天或撤销已执行操作。', { title: '删除技能', danger: true, confirmText: '删除' }))
    await change('agent.skills.delete', { id: skill.id });
}
watch(() => session.client, () => { items.value = []; revision.value = 0; editing.value = false; selected.value = null; Object.assign(form, { id: '', title: '', content: '' }); void load(); }, { immediate: true });
watch(() => session.connected, connected => { if (connected) void load(); });
</script>

<template>
  <section class="skills-panel">
    <h2>Skills</h2>
    <p>技能提供任务方法，不增加执行权限。新建或编辑后默认停用；启用后从下一次模型请求生效，停用阻止后续读取，不撤回模型已读到的内容。</p>
    <p v-if="!available" role="status">连接设备并以主人身份管理技能。</p>
    <p v-if="error" class="error" role="alert">{{ error }}</p>
    <div class="actions"><MdButton :disabled="!available || busy" @click="create">新建技能</MdButton><MdButton variant="text" :disabled="!available || busy" @click="load">刷新清单</MdButton></div>
    <form v-if="editing" class="editor" @submit.prevent="canSave && change('agent.skills.save', { ...form })">
      <h3>{{ selected?.source === 'builtin' ? '内置技能原文' : selected ? '编辑自定义技能' : '创建自定义技能' }}</h3>
      <MdTextField v-model="form.id" label="技能 ID" placeholder="小写字母、数字、短横线或下划线" :disabled="!!selected || busy" />
      <MdTextField v-model="form.title" label="技能名称" :disabled="selected?.source === 'builtin' || busy" />
      <label>技能正文<textarea v-model="form.content" aria-label="技能正文" rows="10" :readonly="selected?.source === 'builtin'" :disabled="busy" /></label>
      <p>{{ contentBytes }} / 4096 字节 · 正文按纯文本展示，不执行其中代码。</p>
      <div class="actions"><MdButton v-if="selected?.source !== 'builtin'" :disabled="!canSave">保存并停用</MdButton><MdButton type="button" variant="text" @click="editing = false">关闭编辑器</MdButton></div>
    </form>
    <p v-if="!items.length && !busy">尚未取得技能清单。</p>
    <article v-for="skill in items" :key="skill.id" class="skill">
      <div><strong>{{ skill.title }}</strong><p>{{ skill.source === 'builtin' ? '内置 · 原文只读' : '自定义' }} · {{ skill.enabled ? '已启用' : '已停用' }}</p><code>{{ skill.id }}</code></div>
      <div class="actions"><MdButton variant="text" :disabled="!available || busy" @click="open(skill)">{{ skill.source === 'builtin' ? '查看原文' : '编辑' }}</MdButton><MdButton variant="tonal" :disabled="!available || busy" @click="toggle(skill)">{{ skill.enabled ? '停用' : '启用' }}</MdButton><MdButton v-if="skill.source === 'custom'" variant="text" :disabled="!available || busy" @click="remove(skill)">删除</MdButton></div>
    </article>
  </section>
</template>

<style scoped>
.skills-panel { line-height: 1.6; max-width: 960px; margin: 0 auto; width: 100%; }
h2 { font: 600 22px var(--font-title); }
.actions { display: flex; gap: 8px; flex-wrap: wrap; }
.skill { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 12px; padding: 18px 0; border-bottom: 1px solid var(--md-outline-variant); }
.skill > div { min-width: 0; overflow-wrap: anywhere; }
.skill p { margin: 4px 0; }
.editor { display: flex; flex-direction: column; gap: 12px; padding: 20px 0; }
textarea { display: block; width: 100%; box-sizing: border-box; padding: 12px; border-radius: 8px; background: var(--md-surface-container); color: var(--md-on-surface); border: 1px solid var(--md-outline); font: 15px/1.6 var(--font-body); resize: vertical; }
.error { color: var(--md-error); overflow-wrap: anywhere; }
</style>
