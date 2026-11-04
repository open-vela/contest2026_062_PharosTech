import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
import { createPinia, setActivePinia } from 'pinia';
import { useLightingStore } from '../src/stores/lighting';
import { LIGHT_ZONES, effectsFor, lightingStatus } from '../src/lib/lighting';
const fixture = vi.hoisted(() => ({ session: undefined as any, poll: undefined as any }));
vi.mock('../src/stores/eye', () => ({ useEyeStore: () => ({ lastState: { appearance: { irisLeft: '#6fd3ff', irisRight: '#6fd3ff' } } }) }));
vi.mock('../src/stores/session', () => ({ useSessionStore: () => fixture.session }));
vi.mock('../src/composables/usePageVisible', () => ({ useVisiblePoll: (fn: () => void) => { fixture.poll = fn; } }));
vi.mock('@nyabula/ui', async () => {
 const { defineComponent, h } = await import('vue');
 return {
  UiIcon: { render: () => h('i') }, describeError: (e: any) => e.message || String(e),
  NkHeader: defineComponent({ props: ['title','subtitle'], setup: p => () => h('header', [p.title,p.subtitle]) }),
  MdButton: defineComponent({ props: { disabled: Boolean }, setup: (p,c) => () => h('button', { ...c.attrs, disabled: p.disabled }, c.slots.default?.()) }),
  NkToggleRow: defineComponent({ props: ['modelValue','disabled','title'], emits: ['update:modelValue'], setup: (p,c) => () => h('button', { 'aria-label': p.title, disabled: p.disabled, onClick: () => c.emit('update:modelValue', !p.modelValue) }, p.modelValue ? '开启' : '关闭') }),
  NkSliderRow: defineComponent({ props: ['modelValue','disabled','title','max'], emits: ['update:modelValue','commit'], setup: (p,c) => () => h('input', { type: 'range', max: p.max, value: p.modelValue, disabled: p.disabled, 'aria-label': p.title, onInput: (e:any) => c.emit('update:modelValue', Number(e.target.value)), onChange: (e:any) => c.emit('commit', Number(e.target.value)) }) }),
  NkColorSwatch: defineComponent({ props: ['modelValue','disabled'], emits: ['update:modelValue'], setup: (p,c) => () => h('button', { disabled: p.disabled, onClick: () => c.emit('update:modelValue','#ff7768') }, '预设红色') }),
 };
});
import LightingFeature from '../src/views/services/features/LightingFeature.vue';
import LightingMini from '../src/views/services/features/LightingMini.vue';
let app: ReturnType<typeof createApp> | undefined, host: HTMLElement, server: any;
const clone = (value:any) => JSON.parse(JSON.stringify(value));
const initial = () => ({ available:true,running:true,sleeping:false,phase:'ready',preset:'custom',activity:'idle',maxBrightness:100,revision:0,appliedRevision:0,frames:1,lastWriteMs:10,lastError:0,musicActive:false,musicLevel:0,musicSource:'',groups:LIGHT_ZONES.map((z,i)=>({...z,enabled:false,color:'#6fd3ff',brightness:12,speed:2,effect:'steady',colorMode:i<2?'iris':'fixed',outputColor:'#000000'})),pixels:Array(33).fill(0) });
async function flush(){for(let i=0;i<5;i++){await Promise.resolve();await nextTick();}}
async function mount(component:any){const pinia=createPinia();setActivePinia(pinia);app=createApp({render:()=>h(component,{type:'lighting',ff:'desktop',active:false,payload:null})});app.use(pinia);app.mount(host);await flush();}
const button=(label:string)=>[...host.querySelectorAll<HTMLButtonElement>('button')].find(el=>el.textContent?.trim()===label)!;
beforeEach(()=>{
 host=document.createElement('div');document.body.append(host);server=initial();
 fixture.session=reactive({connected:true,canControl:true,client:{capabilities:['core.lighting-v1']},request:vi.fn(async(topic:string,data:any)=>{
  if(topic==='lighting.set'){
   for(const group of server.groups)if(data.group==='all'||data.group===group.id){for(const key of ['enabled','color','brightness','effect','colorMode','speed'])if(key in data)group[key]=data[key];if(data.color)group.colorMode='fixed';}
   server.revision++;
  } else if(topic==='lighting.preset'){server.preset=data.name;server.groups.forEach((g:any)=>g.enabled=true);server.revision++;}
  return clone(server);
 })});
});
afterEach(()=>{app?.unmount();app=undefined;host.remove();});
it('addresses only the chosen ear and commits zero brightness on release',async()=>{
 await mount(LightingFeature);
 (host.querySelectorAll('.zone-tabs button')[1] as HTMLButtonElement).click();await flush();
 (host.querySelector('[aria-label="右耳灯光"]') as HTMLButtonElement).click();await flush();
 expect(fixture.session.request).toHaveBeenCalledWith('lighting.set',{group:'right_ear',enabled:true,revision:0});
 expect(server.groups[0].enabled).toBe(false);
 const range=host.querySelector<HTMLInputElement>('input[type=range]')!;expect(range.max).toBe('100');
 const calls=fixture.session.request.mock.calls.filter((c:any[])=>c[0]==='lighting.set').length;
 range.value='0';range.dispatchEvent(new Event('input'));await flush();expect(fixture.session.request.mock.calls.filter((c:any[])=>c[0]==='lighting.set')).toHaveLength(calls);
 range.dispatchEvent(new Event('change'));await flush();expect(server.groups[1].brightness).toBe(0);
});
it('changes iris color mode and offers base-only effects only on the base',async()=>{
 await mount(LightingFeature);button('瞳色互补').click();await flush();expect(server.groups[0].colorMode).toBe('complement');
 expect(button('环绕')).toBeUndefined();
 (host.querySelectorAll('.zone-tabs button')[3] as HTMLButtonElement).click();await flush();button('环绕').click();await flush();expect(server.groups[3].effect).toBe('orbit');
 expect(effectsFor('left_ear').some(e=>e.id==='status')).toBe(false);
});
it('keeps errors visible after a healthy status read',async()=>{
 await mount(LightingFeature);const original=fixture.session.request.getMockImplementation();fixture.session.request.mockImplementation((topic:string,data:any)=>topic==='lighting.set'?Promise.reject(Error('灯带写入失败')):original(topic,data));
 (host.querySelector('[aria-label="左耳灯光"]') as HTMLButtonElement).click();await flush();await useLightingStore().refresh();await flush();expect(host.textContent).toContain('灯带写入失败');
});
it('does not control missing firmware or a read-only session',async()=>{
 fixture.session.client.capabilities=[];await mount(LightingMini);expect(host.textContent).toContain('尚未提供');expect(fixture.session.request).not.toHaveBeenCalled();
 await useLightingStore().set('all',{enabled:true});expect(fixture.session.request).not.toHaveBeenCalled();
});
it('allows all-off during sleep but blocks activation',async()=>{
 server.sleeping=true;server.phase='asleep';server.groups[0].enabled=true;await mount(LightingFeature);
 expect((host.querySelector('[aria-label="左耳灯光"]') as HTMLButtonElement).disabled).toBe(true);
 expect(button('全部关闭').disabled).toBe(false);button('全部关闭').click();await flush();expect(server.groups.every((g:any)=>!g.enabled)).toBe(true);
});
it('supports compact per-zone controls and atomically selected presets',async()=>{
 await mount(LightingMini);expect(host.querySelector('.lighting-preview')).toBeNull();expect(host.querySelector('.mini-art')).toBeNull();(host.querySelector('[aria-label="打开胸口灯光"]') as HTMLButtonElement).click();await flush();expect(server.groups[2].enabled).toBe(true);expect(server.groups[0].enabled).toBe(false);
 await useLightingStore().preset('party');expect(server.preset).toBe('party');
});
it('rejects malformed state instead of showing fabricated light data',()=>{
 expect(lightingStatus(initial())).toBe(true);const bad=initial();bad.groups[0].brightness=101;expect(lightingStatus(bad)).toBe(false);
});
