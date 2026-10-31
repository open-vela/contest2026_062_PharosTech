import { computed, ref } from 'vue';
import type { EyeState } from '@nyabula/eye-engine';
export interface Plugin { id: string; name: string; version: string; state: string; lastError: string; generation: number; permissions: {name:string;granted:boolean}[] }
export interface LogEntry { sequence: number; time: string; pluginId: string; message: string }
export interface Snapshot {
  device: { deviceId: string; pairCode: string; claimCode: string; hasOwner: boolean; pairedTokens: number; uptimeSeconds: number; listenPort: number; pairingActive: boolean; stateFile: string; cloudTransport: string; cloud: {enabled:boolean;url:string}; eye: EyeState; modes:string[]; scenes:string[]; clients: {id:number;name:string;role:string;authenticated:boolean}[]; members:{id:string;name:string;role:string}[]; config: {name:string;pairingEnabled:boolean;battery:{level:number;charging:boolean};wifi:{ssid:string;rssi:number;connected:boolean};uptimeSeconds:number;demo:boolean} };
  plugins: {plugins:Plugin[]}; fixtures:{mocks:Record<string,unknown>;faults:Record<string,string>}; logs:LogEntry[]; revision:number; tools:string[]; runtime:{engine:string;eventDeadlineMs:number;memoryIsolation:boolean};
}
const snapshot = ref<Snapshot | null>(null);
const connected = ref(false);
const selected = ref('');
const section = ref('plugins');
const role = ref('owner');
const notice = ref('');
const failure = ref('');
const busy = ref(false);
let polling = false;
const token = document.querySelector<HTMLMetaElement>('meta[name="nyabula-debug"]')?.content ?? '';
async function api(path:string, body?:unknown) {
  const response = await fetch(path,{method:body===undefined?'GET':'POST',headers:{'X-Nyabula-Debug-Token':token,...(body===undefined?{}:{'Content-Type':'application/json'})},body:body===undefined?undefined:JSON.stringify(body)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error ?? `HTTP ${response.status}`);
  return data.result;
}
async function refresh() {
  if (polling) return;
  polling = true;
  try { snapshot.value = await api('/debug/api/state'); connected.value=true; const plugins=snapshot.value?.plugins.plugins??[];if(!plugins.some(p=>p.id===selected.value))selected.value=plugins[0]?.id??''; }
  catch { connected.value=false; }
  finally { polling=false; }
}
async function call(tool:string,args:Record<string,unknown> = {}) { return api('/debug/api/call',{tool,arguments:args}); }
async function act(tool:string,args:Record<string,unknown>,message='已应用') {
  busy.value=true;failure.value='';notice.value='';
  try {const value=await call(tool,args);if(value?.type==='err')throw new Error(value.data?.message??value.data?.code);notice.value=message;await refresh();return value;}
  catch(e){failure.value=e instanceof Error?e.message:String(e);return null;}
  finally{busy.value=false;}
}
export function useWorkbench(){return{snapshot,connected,selected,section,role,notice,failure,busy,refresh,call,act,plugins:computed(()=>snapshot.value?.plugins.plugins??[]),plugin:computed(()=>snapshot.value?.plugins.plugins.find(p=>p.id===selected.value))};}
export const statusLabel=(value:string)=>({running:'运行中',stopped:'已停止',failed:'运行失败',owner:'主人',family:'家人',guest:'访客'}[value]??value);
