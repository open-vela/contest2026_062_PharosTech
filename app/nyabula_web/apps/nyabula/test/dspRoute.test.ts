import { beforeEach,afterEach,expect,it,vi } from 'vitest';
import { createApp,reactive,nextTick } from 'vue';
import DspControl from '../src/views/services/features/DspControl.vue';
import { eqPreset } from '../src/lib/dspEqualizer';
const fixture=vi.hoisted(()=>({session:undefined as any,poll:undefined as any,suppressed:false}));
vi.mock('../src/stores/session',()=>({useSessionStore:()=>fixture.session}));
vi.mock('../src/composables/usePageVisible',()=>({useVisiblePoll:(cb:any)=>{fixture.poll=cb;}}));
let app:ReturnType<typeof createApp>,host:HTMLElement;
async function flush(){for(let i=0;i<7;i++){await Promise.resolve();await nextTick();}}
beforeEach(()=>{fixture.suppressed=true;fixture.session=reactive({connected:true,canControl:true,client:{},request:vi.fn(async(topic:string)=>topic==='dsp.status'?{bassSuppressed:fixture.suppressed,bassPresent:true,settingsSaved:true,running:false,stopping:false,track:'',source:'',sampleRate:48000,processedFrames:0,outputPeak:0,usbClockPpm:0,usbUnderruns:0,lastError:0,config:{eqEnabled:true,bassEnabled:true,bypass:false,masterDb:-12,crossoverHz:120,satelliteHighpassHz:120,bassLowpassHz:120,eqDb:[0,0,0,0],eqBands16:eqPreset('flat'),outputGainDb:[0,0,0],delayMs:[0,0,0],invert:[false,false,false]}}:{items:[]})});host=document.createElement('div');document.body.append(host);});
afterEach(()=>{app?.unmount();host.remove();});
it('disables 2.1 controls only while actual route is headphones',async()=>{app=createApp(DspControl,{mode:'subwoofer'});app.mount(host);await flush();expect(host.textContent).toContain('当前仅耳机输出');expect([...host.querySelectorAll('input')].every(i=>i.disabled)).toBe(true);const calls=fixture.session.request.mock.calls.length;fixture.suppressed=false;fixture.poll();await flush();expect(host.textContent).not.toContain('当前仅耳机输出');expect([...host.querySelectorAll('input')].some(i=>!i.disabled)).toBe(true);expect(fixture.session.request.mock.calls.slice(calls).every((c:any[])=>c[0]==='dsp.status')).toBe(true);});
it('keeps EQ editable in headphones mode',async()=>{app=createApp(DspControl,{mode:'eq'});app.mount(host);await flush();expect((host.querySelector('[aria-label="频段频率 Hz"]') as HTMLInputElement).disabled).toBe(false);});
