<script setup lang="ts">
import { useRouter } from 'vue-router';
import { MdButton,UiIcon } from '@nyabula/ui';
import { FeaturePageHeader } from '../featureHeader';
import { useIdentityStore } from '../../../stores/identity';
import { useVisionStore } from '../../../stores/vision';
import { useSessionStore } from '../../../stores/session';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import type { FeatureProps } from './contract';
defineProps<FeatureProps>();
const identity=useIdentityStore(),vision=useVisionStore(),session=useSessionStore(),router=useRouter();
useVisiblePoll(()=>{void identity.refresh();void vision.refresh();},1000);
function manage(){void router.push({name:'service',params:{key:session.deviceKey??'',type:'identity'}});}
</script>
<template>
 <div class="presence-feature"><FeaturePageHeader icon="presence" title="在场" subtitle="与身份登记共用家人档案"/>
  <section class="ui-panel presence-card"><div class="presence-heading"><div><h2>{{ vision.state?.running&&vision.state.identifying?`最近见到 ${identity.recent.length} 位家人`:'身份识别尚未开启' }}</h2><p>显示近 3 秒在镜头前识别到的家人；未入镜不代表离家。</p></div><MdButton variant="tonal" @click="manage">管理身份<UiIcon name="chevron_right" :size="17"/></MdButton></div>
   <p v-if="identity.error||identity.readError" class="presence-error" role="alert">{{ identity.error||identity.readError }}</p>
   <div class="presence-members"><article v-for="member in identity.state?.members??[]" :key="member.id" :class="{recent:identity.recent.some(m=>m.id===member.id)}"><span class="presence-avatar">{{ member.name[0] }}</span><div><strong>{{ member.name }}</strong><small>{{ identity.recent.some(m=>m.id===member.id)?'刚刚见到':'镜头前未确认' }}</small></div><i/></article></div>
   <p v-if="!identity.state?.members.length" class="presence-empty">先在身份页登记家人，之后这里会自动更新。</p>
  </section>
 </div>
</template>
<style scoped>
.presence-feature{display:grid;gap:18px}.presence-card{padding:22px;display:grid;gap:20px}.presence-heading{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}.presence-heading h2{margin:0;font-size:20px}.presence-heading p,.presence-empty{margin:7px 0 0;color:var(--md-on-surface-variant);font-size:13px;line-height:1.6}.presence-members{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:12px}.presence-members article{display:flex;align-items:center;gap:12px;padding:16px;border:1px solid var(--md-outline-variant);border-radius:var(--radius-l);background:var(--md-surface-container)}.presence-members article.recent{background:var(--md-secondary-container);border-color:var(--md-primary)}.presence-avatar{width:44px;height:44px;display:grid;place-items:center;flex:none;border-radius:50%;background:var(--md-surface-container-highest);font-size:18px;font-weight:700}.presence-members article>div{flex:1;display:grid;gap:4px}.presence-members small{color:var(--md-on-surface-variant);font-size:12px}.presence-members i{width:7px;height:7px;border-radius:50%;background:var(--md-outline)}.presence-members .recent i{background:var(--md-primary)}.presence-error{color:var(--md-error);margin:0}
</style>
