<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { describeError, MdButton, MdCard } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
const session = useSessionStore();
interface Member { id: string; name: string; role: 'family' | 'guest' }
const members = ref<Member[]>([]);
const name = ref('');
const role = ref<'family' | 'guest'>('family');
const token = ref('');
const error = ref('');
const busy = ref(false);
async function request(topic: string, data = {}) {
  busy.value = true; error.value = '';
  try {
    const result = await session.request(topic, data);
    members.value = (result.members ?? []) as Member[];
    if (typeof result.token === 'string') { token.value = result.token; name.value = ''; }
  } catch (e) { error.value = describeError(e); }
  finally { busy.value = false; }
}
async function revoke(member: Member) {
  if (window.confirm(`撤销「${member.name}」的设备访问权限？`)) await request('access.revoke', { memberId: member.id });
}
onMounted(() => { if (session.isOwner) void request('access.members'); });
</script>
<template>
  <MdCard title="家庭成员">
    <p>主人管理设备与授权；家人可以使用眼睛控制、音乐、闹钟、计时器和已授权插件；访客仅查看设备状态与插件页面。</p>
    <p v-if="!session.isOwner" class="muted">只有设备主人可以管理成员。</p>
    <template v-else>
      <form class="invite" @submit.prevent="request('access.create', { name, role })">
        <label>成员名称<input v-model="name" class="field" required maxlength="63" placeholder="例如：小林" /></label>
        <label>身份<select v-model="role"><option value="family">家人</option><option value="guest">访客</option></select></label>
        <MdButton :disabled="busy || !name.trim()" type="submit">创建访问凭证</MdButton>
      </form>
      <div v-if="token" class="credential">
        <strong>请私下交给这位成员，仅显示这一次</strong>
        <textarea :value="token" readonly aria-label="成员访问凭证" rows="3" />
        <MdButton variant="text" @click="token = ''">已保存，隐藏凭证</MdButton>
      </div>
      <p v-if="error" role="alert">{{ error }}</p>
      <div v-for="member in members" :key="member.id" class="member">
        <strong>{{ member.name }}</strong>
        <select :value="member.role" :disabled="busy" :aria-label="`${member.name}的身份`" @change="request('access.role', { memberId: member.id, role: ($event.target as HTMLSelectElement).value })"><option value="family">家人</option><option value="guest">访客</option></select>
        <MdButton variant="text" :disabled="busy" @click="revoke(member)">撤销</MdButton>
      </div>
      <p v-if="!members.length && !busy && !error" class="muted">尚未添加其他成员。</p>
    </template>
  </MdCard>
</template>
<style scoped>
p { line-height: 1.6; }
.invite { display: flex; gap: 12px; align-items: end; flex-wrap: wrap; }
label { display: grid; gap: 6px; flex: 1; min-width: 130px; }
input, select, textarea { min-width: 0; }
.member { display: flex; align-items: center; gap: 12px; padding: 14px 0; border-bottom: 1px solid var(--md-outline-variant); }
.member strong { flex: 1; overflow-wrap: anywhere; }
.credential { display: grid; gap: 12px; padding: 16px; margin-top: 16px; background: var(--md-secondary-container); border-radius: 12px; }
[role=alert] { color: var(--md-error); }
</style>
