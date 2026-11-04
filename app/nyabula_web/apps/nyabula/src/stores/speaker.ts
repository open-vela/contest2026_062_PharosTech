import { computed, ref, shallowRef, watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from './session';
export interface SpeakerState {
 installed:boolean;ready:boolean;registered:boolean;enabled:boolean;busy:boolean;
 enrolling:boolean;testing:boolean;phase:string;takes:number;required:number;
 kept:number;cohesion:number;score:number;threshold:number;matched:boolean;
 lastError:number;revision:number;
}
export function isSpeakerState(value:unknown):value is SpeakerState {
 const s=value as SpeakerState;
 return !!s&&['installed','ready','registered','enabled','busy','enrolling','testing','matched'].every(k=>typeof (s as any)[k]==='boolean')
  &&typeof s.phase==='string'&&Number.isSafeInteger(s.revision)&&s.revision>=0
  &&Number.isInteger(s.takes)&&s.takes>=0&&Number.isInteger(s.required)&&s.required>0
  &&Number.isFinite(s.score)&&Number.isFinite(s.threshold)&&Number.isInteger(s.lastError);
}
export const useSpeakerStore=defineStore('speaker',()=>{
 const session=useSessionStore(),state=shallowRef<SpeakerState>();
 const busy=ref(false),error=ref(''),readError=ref('');let generation=0,reading=false;
 const supported=computed(()=>session.client?.capabilities.includes('core.speaker-v1')===true);
 const canWrite=computed(()=>session.connected&&session.isOwner&&supported.value&&!!state.value&&!busy.value);
 const active=computed(()=>!!state.value&&(state.value.busy||state.value.enrolling||state.value.testing));
 const label=computed(()=>!session.connected?'设备离线':!session.isOwner?'仅主人可登记和设置':!supported.value?'固件尚未提供声纹服务':!state.value?'正在读取声纹状态…':!state.value.installed?'请先安装声纹模型':state.value.registered?'已登记主人声纹':'尚未登记主人声纹');
 const hint=computed(()=>{
  const s=state.value;if(!s)return '';
  if(s.phase==='error')return s.lastError===-110?'等待说话超时，请重新开始。':`声纹处理未完成（${s.lastError}），请重试。`;
  if(s.phase==='saved')return '声纹已保存在设备。可以先测试，再开启仅回应主人。';
  if(s.phase==='matched')return '声音匹配主人声纹。';
  if(s.phase==='unknown')return '未匹配主人声纹。';
  if(s.phase==='listening')return s.enrolling?`请说当前唤醒词，已采集 ${s.takes} / ${s.required} 次。`:'请说一次当前唤醒词。';
  if(s.busy)return '正在处理，请稍候…';
  return '';
 });
 async function refresh(){
  if(!session.connected||!session.isOwner||!supported.value||reading||busy.value)return;
  reading=true;const epoch=generation,client=session.client;
  try{const value=await session.request('speaker.status');if(!isSpeakerState(value))throw Error('设备返回了无效的声纹状态');if(epoch===generation&&client===session.client){state.value=value;readError.value='';}}
  catch(e){if(epoch===generation&&client===session.client)readError.value=describeError(e);}
  finally{reading=false;}
 }
 async function action(topic:string,data:Record<string,unknown>={}){
  if(!canWrite.value||!state.value)return false;
  const epoch=++generation,client=session.client;busy.value=true;error.value='';
  try{const value=await session.request(topic,{...data,revision:state.value.revision});if(epoch!==generation||client!==session.client)return false;if(!isSpeakerState(value))throw Error('设备未确认声纹操作');state.value=value;return true;}
  catch(e){if(epoch===generation&&client===session.client)error.value=describeError(e);return false;}
  finally{if(epoch===generation){busy.value=false;void refresh();}}
 }
 watch(()=>[session.client,session.connected,session.isOwner,supported.value],()=>{generation++;state.value=undefined;busy.value=false;error.value='';readError.value='';void refresh();},{immediate:true});
 return {state,busy,error,readError,supported,canWrite,active,label,hint,refresh,
  enroll:()=>action('speaker.enroll.start'),test:()=>action('speaker.test.start'),
  cancel:()=>action('speaker.cancel'),remove:()=>action('speaker.delete'),
  configure:(enabled:boolean)=>action('speaker.configure',{enabled})};
});
