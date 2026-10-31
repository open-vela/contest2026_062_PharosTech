import {beforeEach,describe,expect,it,vi} from 'vitest';
import {createPinia,setActivePinia} from 'pinia';
import {nextTick,reactive} from 'vue';
const state=vi.hoisted(()=>({session:null as any,ok:vi.fn(),error:vi.fn()}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>state.session}));
vi.mock('@nyabula/ui',()=>({useToastStore:()=>({ok:state.ok,error:state.error}),permissionZh:(s:string)=>s}));
vi.mock('@nyabula/nyaui-vue',()=>({applyPatches:()=>true}));
import {usePluginsStore} from '../src/stores/plugins';
const snapshot=(flags=[false,false,false])=>({plugins:[{id:'test',name:'Test',permissions:flags.map((granted,i)=>({name:`p${i}`,granted}))}]});
function deferred(){let resolve!:(v:any)=>void,reject!:(e:any)=>void;const promise=new Promise<any>((a,b)=>{resolve=a;reject=b;});return {promise,resolve,reject};}
const tick=async()=>{await Promise.resolve();await nextTick();};
describe('permission write queue',()=>{
 beforeEach(()=>{setActivePinia(createPinia());state.session=reactive({client:{on:()=>()=>{}},request:vi.fn().mockResolvedValue(snapshot())});});
 it('serializes grants and preserves queued positions across refreshes',async()=>{
  const s=usePluginsStore();await s.refresh();const writes:ReturnType<typeof deferred>[]=[];const stale=deferred();
  state.session.request.mockImplementation((t:string)=>{if(t==='plugin.list')return stale.promise;const d=deferred();writes.push(d);return d.promise;});
  const a=s.setPermission('test','p0',true),b=s.setPermission('test','p1',true),c=s.setPermission('test','p2',true);
  expect(writes).toHaveLength(1);expect(s.permissionQueueLength).toBe(3);
  const r=s.refresh();stale.resolve(snapshot());await r;expect(s.list[0].permissions?.map(p=>p.granted)).toEqual([true,true,true]);
  writes[0].resolve(snapshot([true,false,false]));await a;await tick();expect(writes).toHaveLength(2);expect(s.list[0].permissions?.map(p=>p.granted)).toEqual([true,true,true]);
  writes[1].resolve(snapshot([true,true,false]));await b;await tick();writes[2].resolve(snapshot([true,true,true]));await c;expect(s.permissionQueueLength).toBe(0);
 });
 it('ignores a list response captured before a successful grant',async()=>{
  const s=usePluginsStore();await s.refresh();const old=deferred(),write=deferred();state.session.request.mockImplementation((t:string)=>t==='plugin.list'?old.promise:write.promise);
  const r=s.refresh(),g=s.setPermission('test','p0',true);write.resolve(snapshot([true,false,false]));await g;old.resolve(snapshot());await r;expect(s.list[0].permissions?.[0].granted).toBe(true);
 });
 it('rolls back one failed cell and continues the queue',async()=>{
  const s=usePluginsStore();await s.refresh();const a=deferred(),b=deferred();state.session.request.mockReturnValueOnce(a.promise).mockReturnValueOnce(b.promise);
  const x=s.setPermission('test','p0',true),y=s.setPermission('test','p1',true);a.reject(Error('failed'));expect(await x).toBe(false);await tick();expect(s.list[0].permissions?.map(p=>p.granted)).toEqual([false,true,false]);b.resolve({ok:true});expect(await y).toBe(true);expect(s.list[0].permissions?.[1].granted).toBe(true);
 });
 it('keeps the latest intent for repeated changes to one cell',async()=>{
  const s=usePluginsStore();await s.refresh();const a=deferred(),b=deferred();state.session.request.mockReturnValueOnce(a.promise).mockReturnValueOnce(b.promise);
  const x=s.setPermission('test','p0',true),y=s.setPermission('test','p0',false);a.resolve(snapshot([true,false,false]));await x;await tick();expect(s.list[0].permissions?.[0].granted).toBe(false);b.resolve(snapshot());await y;expect(s.list[0].permissions?.[0].granted).toBe(false);
 });
 it('never replays queued grants onto a different device',async()=>{
  const s=usePluginsStore();await s.refresh();const a=deferred();state.session.request.mockReturnValue(a.promise);const x=s.setPermission('test','p0',true),y=s.setPermission('test','p1',true);
  state.session.client={on:()=>()=>{}};await nextTick();expect(await x).toBe(false);expect(await y).toBe(false);a.resolve(snapshot([true,false,false]));await tick();expect(s.list).toEqual([]);expect(s.permissionQueueLength).toBe(0);expect(state.session.request.mock.calls.filter(([t]:any[])=>t==='plugin.grant')).toHaveLength(1);
 });
});
