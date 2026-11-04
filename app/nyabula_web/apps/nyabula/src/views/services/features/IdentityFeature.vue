<script setup lang="ts">
import { computed,ref } from 'vue';
import SpeakerControls from './SpeakerControls.vue';
import { MdButton,MdTextField,UiIcon,useDialogStore } from '@nyabula/ui';
import { FeaturePageHeader } from '../featureHeader';
import { useIdentityStore } from '../../../stores/identity';
import { useVisionStore } from '../../../stores/vision';
import { useCamera } from '../../../composables/useCamera';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { validIdentityName,type IdentityMember } from '../../../lib/identity';
import type { FeatureProps } from './contract';
defineProps<FeatureProps>();
const identity=useIdentityStore(),vision=useVisionStore(),dialog=useDialogStore();
const camera=useCamera(),{video,streaming,received}=camera;
const name=ref('');
const identifying=computed(()=>vision.state?.running&&vision.state.identifying);
const collecting=computed(()=>identity.state?.phase==='collecting');
const canEnroll=computed(()=>identity.canWrite&&identifying.value&&vision.state?.phase==='detecting'&&!collecting.value&&validIdentityName(name.value));
useVisiblePoll(()=>{void identity.refresh();void vision.refresh();},500);
async function enable(){if(vision.state?.running&&!await vision.stop())return;if(await vision.start(false,true)){if(camera.canPreview&&!streaming.value)await camera.start();}}
async function enroll(){if(!canEnroll.value)return;if(camera.canPreview&&!streaming.value)await camera.start();if(await identity.enroll(name.value))name.value='';}
async function remove(member:IdentityMember){if(await dialog.confirm(`删除 ${member.name} 的称呼和本地人脸特征？`,{title:'移除家人',danger:true,confirmText:'移除'}))await identity.remove(member.id);}
async function rename(member:IdentityMember){const next=await dialog.prompt('猫猫该怎么称呼这位家人？',{title:'修改称呼',input:{value:member.name,label:'称呼'}});if(next!==null)await identity.rename(member.id,next);}
</script>
<template>
 <div class="identity-feature" :class="ff">
  <FeaturePageHeader icon="identity" title="身份" :subtitle="identity.label" />
  <div class="identity-grid">
   <section class="ui-panel identity-card">
    <div class="identity-top"><div><h2>认出熟悉的人</h2><p>{{ vision.label }}</p></div><UiIcon name="identity" :size="28" /></div>
    <div class="identity-view"><video v-show="streaming&&received>0" ref="video" muted autoplay playsinline aria-label="身份登记画面"/><div v-if="!streaming||!received" class="identity-empty"><UiIcon name="camera" :size="32"/><span>让一张脸进入画面</span></div><span v-for="(face,index) in streaming&&received?vision.faces:[]" :key="index" class="identity-box" :style="{left:`${face.x*100}%`,top:`${face.y*100}%`,width:`${face.width*100}%`,height:`${face.height*100}%`}"/></div>
    <div class="identity-actions"><MdButton v-if="!identifying" :disabled="!vision.canWrite" @click="enable">{{ vision.state?.running?'切换到身份识别':'开启身份识别' }}</MdButton><MdButton v-else variant="outlined" :disabled="!vision.canWrite" @click="vision.stop">停止识别</MdButton><MdButton v-if="identifying&&!streaming" variant="tonal" :disabled="!camera.canUse.value||!camera.canPreview" @click="camera.start">查看画面</MdButton></div>
    <p class="identity-note">开启后持续在设备本地识别，人脸特征只保存在这台设备。不用于登录授权。</p>
    <p v-if="vision.error||vision.readError||camera.error.value||camera.readError.value" class="identity-error" role="alert">{{ vision.error||vision.readError||camera.error.value||camera.readError.value }}</p>
   </section>
   <section class="ui-panel identity-card">
    <h2>登记家人</h2><p class="identity-note">输入称呼后开始采集，缓慢转动头部，保持画面中只有一张脸。</p>
    <form class="identity-register" @submit.prevent="enroll"><MdTextField v-model="name" label="称呼" placeholder="例如：妈妈" :disabled="collecting"/><MdButton :disabled="!canEnroll" type="submit">登记当前人脸</MdButton></form>
    <div v-if="collecting" class="identity-progress" role="status"><div><strong>正在记住 {{ identity.state?.enrolling }}</strong><span>{{ identity.state?.samples }} / {{ identity.state?.required }}</span></div><progress :value="identity.state?.samples" :max="identity.state?.required"/><MdButton variant="text" :disabled="!identity.canWrite" @click="identity.cancel">取消登记</MdButton></div>
    <p v-if="identity.enrollmentHint" :class="{'identity-error':identity.state?.phase==='error','identity-success':identity.state?.phase==='saved'}" role="status">{{ identity.enrollmentHint }}</p>
    <p v-if="identity.error||identity.readError" class="identity-error" role="alert">{{ identity.error||identity.readError }}</p>
    <div class="identity-members"><div v-for="member in identity.state?.members??[]" :key="member.id" class="identity-member"><span class="identity-avatar">{{ member.name[0] }}</span><div><strong>{{ member.name }}</strong><small>{{ identity.recent.some(m=>m.id===member.id)?'刚刚在镜头前见到':'已登记' }}</small></div><MdButton variant="icon" :aria-label="`修改${member.name}称呼`" :disabled="!identity.canWrite||collecting" @click="rename(member)"><UiIcon name="edit" :size="18"/></MdButton><MdButton variant="icon" :aria-label="`移除${member.name}`" :disabled="!identity.canWrite||collecting" @click="remove(member)"><UiIcon name="delete" :size="18"/></MdButton></div><p v-if="identity.state&&!identity.state.members.length" class="identity-note">还没有登记家人。</p></div>
   </section>
  </div>
  <SpeakerControls />
 </div>
</template>
<style scoped>
.identity-feature{display:grid;gap:18px}.identity-grid{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:18px}.identity-card{display:flex;flex-direction:column;gap:14px;padding:20px;min-width:0}.identity-card h2{margin:0;font-size:19px}.identity-top{display:flex;justify-content:space-between;align-items:center;gap:14px}.identity-top p,.identity-note{margin:0;color:var(--md-on-surface-variant);font-size:13px;line-height:1.65}.identity-top p{margin-top:5px}.identity-view{position:relative;aspect-ratio:4/3;max-height:360px;overflow:hidden;background:#101617;border-radius:var(--radius-m)}.identity-view video{width:100%;height:100%;object-fit:contain;position:absolute;inset:0}.identity-empty{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;color:var(--md-on-surface-variant)}.identity-box{position:absolute;box-sizing:border-box;border:2px solid #65e2ac;border-radius:8px;pointer-events:none}.identity-actions,.identity-register{display:flex;gap:10px;align-items:center;flex-wrap:wrap}.identity-register>:first-child{flex:1;min-width:150px}.identity-error{margin:0;color:var(--md-error);font-size:13px;line-height:1.6}.identity-success{margin:0;color:var(--md-primary)}.identity-progress{display:grid;gap:10px}.identity-progress>div{display:flex;justify-content:space-between;gap:10px}.identity-progress progress{width:100%;height:8px;accent-color:var(--md-primary)}.identity-members{display:grid;gap:10px}.identity-member{display:flex;align-items:center;gap:9px;padding:10px 0;border-top:1px solid var(--md-outline-variant)}.identity-avatar{width:38px;height:38px;flex:none;border-radius:50%;display:grid;place-items:center;background:var(--md-secondary-container);color:var(--md-on-secondary-container);font-weight:700}.identity-member>div{display:grid;gap:3px;flex:1;min-width:0}.identity-member strong{overflow-wrap:anywhere}.identity-member small{font-size:12px;color:var(--md-on-surface-variant)}.identity-feature.phone .identity-grid{grid-template-columns:1fr}
@media(max-width:850px){.identity-grid{grid-template-columns:1fr}}
</style>
