import { computed, ref, shallowRef, watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from './session';
import { OBJECT_TRACKING_RELEASED, isVisionState, type VisionState } from '../lib/vision';
export const useVisionStore=defineStore('vision',()=>{
 const session=useSessionStore(),state=shallowRef<VisionState>();
 const busy=ref(false),error=ref(''),readError=ref('');
 const supported=computed(()=>session.client?.capabilities.includes('core.vision-v1')===true);
 const canWrite=computed(()=>session.connected&&session.isOwner&&supported.value&&!busy.value);
 const faces=computed(()=>session.connected&&state.value?.running&&state.value.ageMs!==undefined&&state.value.ageMs<700?state.value.result?.faces??[]:[]);
 const target=computed(()=>session.connected&&state.value?.running&&state.value.tracking&&state.value.phase!=='lost'&&(state.value.ageMs??Infinity)<700?state.value.result?.target:undefined);
 const trackerSupported=computed(()=>OBJECT_TRACKING_RELEASED&&session.client?.capabilities.includes('core.tracker-v1')===true);
 const label=computed(()=>!session.connected?'设备离线':!supported.value?'固件尚未提供本地视觉服务':state.value?.phase==='loading'?(state.value.tracking?'正在加载物品追踪模型':state.value.identifying?'正在加载身份模型':'正在加载人脸检测模型'):state.value?.phase==='error'?`视觉分析未启动（${state.value.lastError}）`:state.value?.tracking&&state.value.running?(state.value.phase==='lost'?'目标暂时丢失 · 猫眼暂停跟随':state.value.phase==='learning'?`学习中 ${state.value.result?.progress??0}% · 缓慢转动物品`:'已记住 · 正在跟随物品'):state.value?.running?`${state.value.identifying?'身份识别':state.value.following?'猫眼跟随':'人脸检测'} · ${faces.value.length} 张脸`:'在设备本地检测人脸');
 let version=0,reading=false;
 async function refresh(){
  if(!session.connected||!session.isOwner||!supported.value||busy.value||reading)return;
  reading=true;const epoch=version,client=session.client;
  try{const next=await session.request('vision.status');if(!isVisionState(next))throw Error('设备返回了无效的视觉结果');if(epoch===version&&client===session.client){state.value=next;readError.value='';}}
  catch(e){if(epoch===version&&client===session.client)readError.value=describeError(e);}
  finally{reading=false;}
 }
 async function action(topic:string,data:Record<string,unknown>={}){
  if(!canWrite.value)return false;
  const client=session.client,epoch=++version;busy.value=true;error.value='';
  try{const next=await session.request(topic,data);if(epoch!==version||client!==session.client)return false;if(!isVisionState(next))throw Error('设备未确认视觉状态');state.value=next;return true;}
  catch(e){if(epoch===version&&client===session.client)error.value=describeError(e);return false;}
  finally{if(epoch===version){busy.value=false;void refresh();}}
 }
 watch(()=>[session.client,session.connected,supported.value],()=>{version++;state.value=undefined;busy.value=false;error.value='';readError.value='';void refresh();},{immediate:true});
 return {state,busy,error,readError,supported,canWrite,faces,target,trackerSupported,label,refresh,learn:(box?:{x:number;y:number;width:number;height:number},follow=true)=>action('vision.learn',{...box,follow}),start:(follow=false,identify=false)=>action('vision.start',{follow,...(identify?{identify:true}:{})}),stop:()=>action('vision.stop')};
});
