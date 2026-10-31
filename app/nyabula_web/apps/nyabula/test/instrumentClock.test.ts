import {afterEach,expect,it} from 'vitest';
import {createApp,h,nextTick,ref} from 'vue';
import DotMatrix from '../src/skins/instrument/parts/DotMatrix.vue';
import desktop from '../src/skins/instrument/shells/InstrumentDesktopShell.vue?raw';
import tablet from '../src/skins/instrument/shells/InstrumentTabletShell.vue?raw';
let cleanup=()=>{};
afterEach(()=>cleanup());
it('blinks only the two colon LEDs without changing geometry or clock label',async()=>{
 const on=ref(true),text=ref('07:59');const host=document.createElement('div');document.body.append(host);
 const app=createApp({render:()=>h(DotMatrix,{text:text.value,dot:3,gap:1.2,colonOn:on.value})});app.mount(host);cleanup=()=>{app.unmount();host.remove();};
 const geometry=()=>({width:host.querySelector('svg')!.getAttribute('width'),height:host.querySelector('svg')!.getAttribute('height'),viewBox:host.querySelector('svg')!.getAttribute('viewBox'),dots:[...host.querySelectorAll('circle')].map(p=>[p.getAttribute('cx'),p.getAttribute('cy'),p.getAttribute('r')])});
 const initial=geometry();
 for(const time of ['00:00','07:59','11:11','23:59']){
  text.value=time;on.value=true;await nextTick();const lit=host.querySelectorAll('circle.on').length;
  on.value=false;await nextTick();expect(host.querySelectorAll('circle.on').length).toBe(lit-2);expect(geometry()).toEqual(initial);expect(host.querySelector('svg')!.getAttribute('aria-label')).toBe(time);
  on.value=true;await nextTick();expect(host.querySelectorAll('circle.on').length).toBe(lit);expect(geometry()).toEqual(initial);
 }
});
it('desktop and tablet keep the colon character and pass its visibility separately',()=>{
 for(const source of [desktop,tablet]){
  expect(source).not.toContain("replace(':', ' ')");
  expect(source).toContain(':colon-on="clock.colonOn()"');
 }
});
