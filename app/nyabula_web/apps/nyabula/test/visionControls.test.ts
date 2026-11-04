import { beforeEach, expect, it, vi } from 'vitest';
import { reactive, nextTick } from 'vue';
import { createPinia, setActivePinia } from 'pinia';
import { useVisionStore } from '../src/stores/vision';
import { isVisionState } from '../src/lib/vision';
const fixture=vi.hoisted(()=>({session:undefined as any}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>fixture.session}));
let server:any;
async function flush(){for(let i=0;i<5;i++){await Promise.resolve();await nextTick();}}
beforeEach(()=>{setActivePinia(createPinia());server={running:false,phase:'idle',following:false,frames:0,lastError:0,followError:0};fixture.session=reactive({connected:true,isOwner:true,client:{capabilities:['core.vision-v1']},request:vi.fn(async(topic:string,data:any)=>{
 if(topic==='vision.start')server={...server,running:true,phase:'loading',following:data.follow};
 if(topic==='vision.stop')server={...server,running:false,phase:'idle'};
 return structuredClone(server);
})});});
it('does not start capture until explicitly requested',async()=>{const store=useVisionStore();await flush();expect(fixture.session.request.mock.calls.map((c:any)=>c[0])).toEqual(['vision.status']);await store.start(true);expect(fixture.session.request).toHaveBeenCalledWith('vision.start',{follow:true});expect(store.state?.following).toBe(true);});
it('hides stale detections and disconnected results',async()=>{const store=useVisionStore();await flush();server={...server,running:true,phase:'detecting',ageMs:50,result:{sequence:1,capturedMs:1,faces:[{x:.1,y:.1,width:.2,height:.2,confidence:.9,landmarks:Array(10).fill(.2)}]}};await store.refresh();expect(store.faces).toHaveLength(1);server.ageMs=800;await store.refresh();expect(store.faces).toHaveLength(0);fixture.session.connected=false;await flush();expect(store.state).toBeUndefined();});
it('refuses malformed face data rather than drawing it',()=>{expect(isVisionState(server)).toBe(true);server.result={sequence:1,capturedMs:1,faces:[{x:Infinity,y:0,width:1,height:1,confidence:1,landmarks:[]}]};server.ageMs=0;expect(isVisionState(server)).toBe(false);});
it('retains a device error without fabricating a running state',async()=>{const store=useVisionStore();await flush();fixture.session.request.mockImplementation(async(topic:string)=>{if(topic==='vision.start')throw Error('摄像头已关闭');return structuredClone(server);});expect(await store.start()).toBe(false);expect(store.state?.running).toBe(false);expect(store.error).toContain('摄像头已关闭');});

it('starts object learning only on an explicit request',async()=>{
 fixture.session.client.capabilities.push('core.tracker-v1');const s=useVisionStore();await flush();
 expect(fixture.session.request).not.toHaveBeenCalledWith('vision.learn',expect.anything());
 await s.learn({x:.2,y:.2,width:.3,height:.4},true);
 expect(fixture.session.request).toHaveBeenCalledWith('vision.learn',{x:.2,y:.2,width:.3,height:.4,follow:true});
});
it('hides a lost or stale object instead of following its old box',async()=>{
 const s=useVisionStore();await flush();server={...server,running:true,tracking:true,phase:'tracking',ageMs:50,result:{sequence:1,capturedMs:1,faces:[],phase:'tracking',progress:100,views:4,target:{x:.2,y:.2,width:.2,height:.2,confidence:.9}}};
 await s.refresh();expect(s.target).toBeDefined();server.phase='lost';await s.refresh();expect(s.target).toBeUndefined();server.phase='tracking';server.ageMs=800;await s.refresh();expect(s.target).toBeUndefined();
});

it('keeps unfinished object tracking out of the product UI',async()=>{fixture.session.client.capabilities.push('core.tracker-v1');const s=useVisionStore();await flush();expect(s.trackerSupported).toBe(false);});
