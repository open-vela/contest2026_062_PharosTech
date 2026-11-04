import { beforeEach,expect,it,vi } from 'vitest';
import { reactive,nextTick } from 'vue';
import { createPinia,setActivePinia } from 'pinia';
import { useSpeakerStore } from '../src/stores/speaker';
const fixture=vi.hoisted(()=>({session:undefined as any}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>fixture.session}));
let server:any;
async function flush(){for(let i=0;i<5;i++){await Promise.resolve();await nextTick();}}
beforeEach(()=>{setActivePinia(createPinia());server={installed:true,ready:false,registered:false,enabled:false,busy:false,enrolling:false,testing:false,phase:'idle',takes:0,required:4,kept:0,cohesion:0,score:-2,threshold:.5,matched:false,lastError:0,revision:0};fixture.session=reactive({connected:true,isOwner:true,client:{capabilities:['core.speaker-v1']},request:vi.fn(async(topic:string,data:any)=>{if(topic==='speaker.enroll.start')server={...server,enrolling:true,busy:true,phase:'loading'};if(topic==='speaker.configure')server={...server,enabled:data.enabled,revision:server.revision+1};return structuredClone(server);})});});
it('does not enroll or enable filtering on page load',async()=>{const s=useSpeakerStore();await flush();expect(s.state?.enabled).toBe(false);expect(fixture.session.request.mock.calls.every((c:any[])=>c[0]==='speaker.status')).toBe(true);await s.enroll();expect(fixture.session.request).toHaveBeenCalledWith('speaker.enroll.start',{revision:0});expect(s.state?.registered).toBe(false);expect(s.state?.enrolling).toBe(true);});
it('does not query biometrics without owner authority',async()=>{fixture.session.isOwner=false;const s=useSpeakerStore();await flush();expect(await s.enroll()).toBe(false);expect(fixture.session.request).not.toHaveBeenCalled();});
it('retains enabled preference after a rejected write',async()=>{server.registered=true;const s=useSpeakerStore();await flush();fixture.session.request.mockImplementation(async(topic:string)=>{if(topic==='speaker.configure')throw Error('写入失败');return structuredClone(server);});expect(await s.configure(true)).toBe(false);expect(s.state?.enabled).toBe(false);expect(s.error).toContain('写入失败');});
it('discards old device responses',async()=>{const s=useSpeakerStore();await flush();let resolve!:(v:any)=>void;fixture.session.request.mockImplementation(()=>new Promise(r=>resolve=r));const pending=s.enroll();fixture.session.connected=false;await flush();resolve({...server,enrolling:true});await pending;expect(s.state).toBeUndefined();});
