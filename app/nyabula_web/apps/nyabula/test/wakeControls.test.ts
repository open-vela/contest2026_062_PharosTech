import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createApp, h, nextTick, reactive } from 'vue';
const state=vi.hoisted(()=>({session:undefined as any,poll:undefined as any}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>state.session}));
vi.mock('../src/composables/usePageVisible',()=>({useVisiblePoll:(poll:()=>void)=>{state.poll=poll;}}));
vi.mock('@nyabula/ui',async()=>{const {defineComponent,h}=await import('vue');return {
 MdButton:defineComponent({props:['disabled'],setup:(p,c)=>()=>h('button',{...c.attrs,disabled:p.disabled},c.slots.default?.())}),
 MdTextField:defineComponent({props:['modelValue','disabled','error'],emits:['update:modelValue'],setup:(p,c)=>()=>h('label',[h('input',{value:p.modelValue,disabled:p.disabled,onInput:(e:any)=>c.emit('update:modelValue',e.target.value)}),p.error])}),
 NkSliderRow:defineComponent({props:['modelValue','disabled'],emits:['update:modelValue'],setup:(p,c)=>()=>h('input',{type:'range',value:p.modelValue,disabled:p.disabled,onInput:(e:any)=>c.emit('update:modelValue',Number(e.target.value))})}),
 NkToggleRow:defineComponent({setup:()=>()=>h('span')}),UiIcon:defineComponent({setup:()=>()=>h('span')}),describeError:(e:Error)=>e.message,
};});
import WakeFeature from '../src/views/services/features/WakeFeature.vue';
import BluetoothMini from '../src/views/services/features/BluetoothMini.vue';
let host:HTMLElement,app:ReturnType<typeof createApp>;
const flush=async()=>{for(let i=0;i<12;i++)await Promise.resolve();await nextTick();};
const snapshot=()=>({enabled:true,running:true,state:'idle',streaming:true,models:{kws:{state:'ready',error:0}},microphone:{open:true,level:500,claims:0},config:{wakeSensitivity:70,wakePhrase:''},errors:{last:0,capture:0,text:''},test:{active:false,mode:'none',state:'done',samples:0,peak:0,detections:0}});
let voice:any;
const button=(text:string)=>[...host.querySelectorAll('button')].find(b=>b.textContent===text)!;
beforeEach(()=>{voice=snapshot();state.session=reactive({connected:true,isOwner:true,client:{},request:vi.fn().mockImplementation((topic:string)=>Promise.resolve(topic==='audio.status'?{input:{muted:false,gainDb:18}}:voice))});host=document.createElement('div');document.body.append(host);});
afterEach(()=>{app?.unmount();host.remove();});
async function mount(component:any){app=createApp({render:()=>h(component)});app.mount(host);await flush();}
describe('wake controls',()=>{
 it('runs isolated keyword and real microphone probes, not a conversation',async()=>{await mount(WakeFeature);button('开始 20 秒测试').click();await flush();expect(state.session.request).toHaveBeenCalledWith('voice.test.start',{mode:'wake',durationMs:20000},{timeoutMs:8000});button('开始 5 秒测试').click();await flush();expect(state.session.request).toHaveBeenCalledWith('voice.test.start',{mode:'microphone',durationMs:5000},{timeoutMs:8000});expect(state.session.request.mock.calls.some(([topic]:any[])=>topic==='voice.listen')).toBe(false);});
 it('keeps unsaved words across polls and rejects invalid characters',async()=>{await mount(WakeFeature);const input=host.querySelector('input')!;input.value='小喵🙂';input.dispatchEvent(new Event('input'));await flush();state.poll();await flush();expect(input.value).toBe('小喵🙂');expect(button('保存设置').disabled).toBe(true);input.value='小喵小喵';input.dispatchEvent(new Event('input'));await flush();button('保存设置').click();await flush();expect(state.session.request).toHaveBeenCalledWith('voice.config',{wakeSensitivity:70,wakePhrase:'小喵小喵'},{timeoutMs:30000});});
 it('blocks probes while playback owns capture',async()=>{voice.microphone.claims=1;await mount(WakeFeature);expect(button('开始 20 秒测试').disabled).toBe(true);expect(button('开始 5 秒测试').disabled).toBe(true);});
 it('preserves action errors after a healthy poll',async()=>{await mount(WakeFeature);state.session.request.mockImplementation((topic:string)=>topic==='voice.test.start'?Promise.reject(Error('capture busy')):Promise.resolve(topic==='audio.status'?{}:voice));button('开始 20 秒测试').click();await flush();state.poll();await flush();expect(host.textContent).toContain('capture busy');});
});
describe('Bluetooth compact control',()=>{
 it('addresses the selected source and keeps command errors',async()=>{const bt={state:'ready',connected:{name:'Laptop',address:'AA'},connections:[{connected:true},{connected:true}],avrcp:{connected:true,playing:true,title:'Track'}};state.session.request.mockResolvedValue(bt);await mount(BluetoothMini);state.poll();await flush();expect(host.textContent).toContain('2 台在线');state.session.request.mockImplementation((t:string)=>t==='bt.media.pause'?Promise.reject(Error('rejected')):Promise.resolve(bt));host.querySelector<HTMLButtonElement>('[aria-label="暂停"]')!.click();await flush();expect(state.session.request).toHaveBeenCalledWith('bt.media.pause',{address:'AA'});state.poll();await flush();expect(host.textContent).toContain('rejected');});
});
