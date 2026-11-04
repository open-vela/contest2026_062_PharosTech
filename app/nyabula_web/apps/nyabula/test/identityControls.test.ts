import { beforeEach,expect,it,vi } from 'vitest';
import { reactive,nextTick } from 'vue';
import { createPinia,setActivePinia } from 'pinia';
import { useIdentityStore } from '../src/stores/identity';
const fixture=vi.hoisted(()=>({session:undefined as any}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>fixture.session}));
let server:any;
async function flush(){for(let i=0;i<5;i++){await Promise.resolve();await nextTick();}}
beforeEach(()=>{setActivePinia(createPinia());server={revision:0,phase:'idle',reason:'',lastError:0,samples:0,required:6,enrolling:'',faces:0,members:[]};fixture.session=reactive({connected:true,isOwner:true,client:{capabilities:['core.identity-v1']},request:vi.fn(async(topic:string,data:any)=>{if(topic==='identity.enroll.start')server={...server,phase:'collecting',enrolling:data.name};return structuredClone(server);})});});
it('never adds a family member merely from a typed name',async()=>{const s=useIdentityStore();await flush();expect(s.state?.members).toEqual([]);await s.enroll(' 妈妈 ');expect(fixture.session.request).toHaveBeenCalledWith('identity.enroll.start',{name:'妈妈',revision:0});expect(s.state?.phase).toBe('collecting');expect(s.state?.members).toEqual([]);});
it('does not expose stale family presence after disconnect',async()=>{server.members=[{id:'person-1',name:'妈妈',recent:true,seenAgoMs:0,confidence:.8}];const s=useIdentityStore();await flush();expect(s.recent).toHaveLength(1);fixture.session.connected=false;await flush();expect(s.recent).toHaveLength(0);expect(s.state).toBeUndefined();});
it('retains stored names when deletion fails',async()=>{server.members=[{id:'person-1',name:'妈妈',recent:false,seenAgoMs:-1,confidence:0}];const s=useIdentityStore();await flush();fixture.session.request.mockImplementation(async(topic:string)=>{if(topic==='identity.remove')throw Error('写入失败');return structuredClone(server);});expect(await s.remove('person-1')).toBe(false);expect(s.state?.members).toHaveLength(1);expect(s.error).toContain('写入失败');});
it('does not query or enroll without owner authority',async()=>{fixture.session.isOwner=false;const s=useIdentityStore();await flush();expect(await s.enroll('测试')).toBe(false);expect(fixture.session.request).not.toHaveBeenCalled();});
