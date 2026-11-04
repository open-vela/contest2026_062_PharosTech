import { beforeEach, expect, it, vi } from 'vitest';
import { reactive, nextTick } from 'vue';
import { createPinia, setActivePinia } from 'pinia';
import { usePrivacyStore } from '../src/stores/privacy';
const fixture = vi.hoisted(() => ({ session: undefined as any }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => fixture.session }));
let server: {camera:boolean;mic:boolean;revision:number};
async function flush() { for(let i=0;i<5;i++) { await Promise.resolve(); await nextTick(); } }
beforeEach(() => {
 setActivePinia(createPinia());server={camera:true,mic:false,revision:2};
 fixture.session=reactive({connected:true,isOwner:true,client:{capabilities:['core.privacy-v1']},request:vi.fn(async(topic:string,data:any) => {
  if(topic==='privacy.set'){expect(data.revision).toBe(server.revision);server={...server,...data,revision:server.revision+1};}return {...server};
 })});
});
it('writes only one switch and waits for the device acknowledgement',async()=>{
 const store=usePrivacyStore();await flush();await store.set('camera',false);await flush();
 expect(fixture.session.request).toHaveBeenCalledWith('privacy.set',{camera:false,revision:2});
 expect(store.state).toEqual({camera:false,mic:false,revision:3});
});
it('does not claim a failed setting was applied',async()=>{
 const store=usePrivacyStore();await flush();fixture.session.request.mockImplementation(async(topic:string)=>{if(topic==='privacy.set')throw Error('保存失败');return {...server};});
 await store.set('camera',false);await flush();expect(store.state?.camera).toBe(true);expect(store.error).toContain('保存失败');
});
it('does not query or write through old firmware or a family session',async()=>{
 fixture.session.client.capabilities=[];const store=usePrivacyStore();await flush();await store.set('camera',false);expect(fixture.session.request).not.toHaveBeenCalled();
 fixture.session.isOwner=false;fixture.session.client.capabilities=['core.privacy-v1'];await flush();await store.set('mic',true);expect(fixture.session.request).not.toHaveBeenCalled();
});
it('discards a response from the previous device',async()=>{
 const store=usePrivacyStore();await flush();let finish!:(v:any)=>void;
 fixture.session.request.mockImplementation(()=>new Promise(resolve=>{finish=resolve;}));const pending=store.set('camera',false);
 fixture.session.connected=false;fixture.session.client={capabilities:[]};await flush();finish({camera:false,mic:false,revision:3});await pending;expect(store.state).toBeUndefined();
});
