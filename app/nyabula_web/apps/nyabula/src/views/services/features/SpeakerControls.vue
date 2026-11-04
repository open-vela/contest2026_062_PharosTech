<script setup lang="ts">
import { MdButton, MdSwitch, useDialogStore } from '@nyabula/ui';
import { useSpeakerStore } from '../../../stores/speaker';
import { usePrivacyStore } from '../../../stores/privacy';
import { useVisiblePoll } from '../../../composables/usePageVisible';
const speaker=useSpeakerStore(),privacy=usePrivacyStore(),dialog=useDialogStore();
useVisiblePoll(()=>{void speaker.refresh();void privacy.refresh();},700);
async function enroll(){
 if(await dialog.confirm(speaker.state?.registered?'重新登记会替换设备里保存的主人声纹。请由本人说话。':'将采集四次唤醒词，在设备本地保存声纹特征。请由本人说话。',{title:'登记主人声纹',confirmText:'开始登记'}))await speaker.enroll();
}
async function remove(){if(await dialog.confirm('删除本地声纹，并关闭“仅回应主人”？',{title:'删除声纹',danger:true,confirmText:'删除'}))await speaker.remove();}
</script>
<template>
 <section class="ui-panel speaker-card">
  <div class="speaker-heading"><h2>认出你的声音</h2><span>{{ speaker.label }}</span></div>
  <p>点击登记后，按提示说四次当前唤醒词，每次之间稍作停顿。声纹特征仅保存在这台设备。</p>
  <p v-if="privacy.state?.mic===false" class="speaker-error" role="status">麦克风已关闭，请先在隐私设置中开启。</p>
  <div v-if="speaker.state?.enrolling" class="speaker-progress"><span>采集进度 {{ speaker.state.takes }} / {{ speaker.state.required }}</span><progress :value="speaker.state.takes" :max="speaker.state.required"/></div>
  <p v-if="speaker.hint" role="status" :class="{'speaker-error':speaker.state?.phase==='error'}">{{ speaker.hint }}</p>
  <div class="speaker-actions">
   <MdButton :disabled="!speaker.canWrite||speaker.active||!speaker.state?.installed||privacy.state?.mic!==true" @click="enroll">{{ speaker.state?.registered?'重新登记':'登记我的声纹' }}</MdButton>
   <MdButton v-if="speaker.state?.registered" variant="outlined" :disabled="!speaker.canWrite||speaker.active||privacy.state?.mic!==true" @click="speaker.test">测试声纹</MdButton>
   <MdButton v-if="speaker.active" variant="text" :disabled="!speaker.canWrite" @click="speaker.cancel">取消</MdButton>
   <MdButton v-else-if="speaker.state?.registered" variant="text" :disabled="!speaker.canWrite" @click="remove">删除声纹</MdButton>
  </div>
  <div class="speaker-policy"><div><strong>仅回应主人</strong><p>开启后，唤醒词还需通过声纹匹配。默认关闭。</p></div><MdSwitch :model-value="speaker.state?.enabled===true" aria-label="仅回应主人" :disabled="!speaker.canWrite||speaker.active||!speaker.state?.registered" @update:model-value="speaker.configure"/></div>
  <p v-if="speaker.error||speaker.readError" class="speaker-error" role="alert">{{ speaker.error||speaker.readError }}</p>
 </section>
</template>
<style scoped>
.speaker-card{display:grid;gap:16px;padding:20px}.speaker-heading,.speaker-policy{display:flex;align-items:center;justify-content:space-between;gap:16px}.speaker-heading h2{margin:0;font-size:19px}.speaker-heading span,.speaker-card p{margin:0;font-size:13px;line-height:1.65;color:var(--md-on-surface-variant)}.speaker-actions{display:flex;gap:10px;flex-wrap:wrap}.speaker-policy{border-top:1px solid var(--md-outline-variant);padding-top:16px}.speaker-policy p{margin-top:5px}.speaker-progress{display:grid;gap:8px;font-size:13px}.speaker-progress progress{width:100%;accent-color:var(--md-primary)}.speaker-card .speaker-error{color:var(--md-error)}@media(max-width:600px){.speaker-heading{align-items:flex-start;flex-direction:column;gap:5px}}
</style>
