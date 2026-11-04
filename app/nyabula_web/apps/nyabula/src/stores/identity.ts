import { computed,ref,shallowRef,watch } from 'vue';
import { defineStore } from 'pinia';
import { describeError } from '@nyabula/ui';
import { useSessionStore } from './session';
import { isIdentityState,IDENTITY_REASONS,validIdentityName,type IdentityState } from '../lib/identity';
export const useIdentityStore=defineStore('identity',()=>{
 const session=useSessionStore(),state=shallowRef<IdentityState>();
 const busy=ref(false),error=ref(''),readError=ref(''),clock=ref(Date.now());
 let version=0,reading=false,receivedAt=0;
 const supported=computed(()=>session.client?.capabilities.includes('core.identity-v1')===true);
 const canWrite=computed(()=>session.connected&&session.isOwner&&supported.value&&!!state.value&&!busy.value);
 const recent=computed(()=>session.connected&&clock.value-receivedAt<3500?state.value?.members.filter(m=>m.recent)??[]:[]);
 const label=computed(()=>!session.connected?'设备离线':!supported.value?'固件尚未提供身份登记':!session.isOwner?'仅主人可查看和登记':!state.value?'正在读取家人档案…':`${state.value.members.length} 位家人已登记`);
 const enrollmentHint=computed(()=>state.value?.phase==='saved'?`已记住 ${state.value.enrolling}`:IDENTITY_REASONS[state.value?.reason??'']??'');
 async function refresh(){clock.value=Date.now();if(!session.connected||!session.isOwner||!supported.value||reading||busy.value)return;reading=true;const epoch=version,client=session.client;
  try{const value=await session.request('identity.status');if(!isIdentityState(value))throw Error('设备返回了无效的身份状态');if(epoch===version&&client===session.client){state.value=value;receivedAt=clock.value=Date.now();readError.value='';}}
  catch(e){if(epoch===version&&client===session.client)readError.value=describeError(e);}finally{reading=false;}
 }
 async function action(topic:string,data:Record<string,unknown>={}){
  if(!canWrite.value||!state.value)return false;
  const client=session.client,epoch=++version;busy.value=true;error.value='';
  try{const value=await session.request(topic,{...data,revision:state.value.revision});if(epoch!==version||client!==session.client)return false;if(!isIdentityState(value))throw Error('设备未确认身份设置');state.value=value;receivedAt=clock.value=Date.now();return true;}
  catch(e){if(epoch===version&&client===session.client)error.value=describeError(e);return false;}
  finally{if(epoch===version){busy.value=false;void refresh();}}
 }
 function enroll(name:string){if(!validIdentityName(name)){error.value='请输入简短的称呼';return Promise.resolve(false);}return action('identity.enroll.start',{name:name.trim()});}
 function rename(id:string,name:string){if(!validIdentityName(name)){error.value='请输入简短的称呼';return Promise.resolve(false);}return action('identity.rename',{id,name:name.trim()});}
 watch(()=>[session.client,session.connected,session.isOwner,supported.value],()=>{version++;state.value=undefined;busy.value=false;error.value='';readError.value='';receivedAt=0;void refresh();},{immediate:true});
 return {state,busy,error,readError,supported,canWrite,recent,label,enrollmentHint,refresh,enroll,rename,remove:(id:string)=>action('identity.remove',{id}),cancel:()=>action('identity.enroll.cancel')};
});
