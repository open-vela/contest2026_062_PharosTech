import {afterEach,beforeEach,expect,it} from 'vitest';
import {createApp,h,nextTick,reactive,ref} from 'vue';
import {useContextColumn} from '../src/composables/useContextColumn';
import {contextPresence} from '../src/composables/contextPresence';
import {readFileSync} from 'node:fs';
import {resolve} from 'node:path';
const baseCss=readFileSync(resolve('src/styles/app.css'),'utf8');
const nebulaCss=readFileSync(resolve('../../packages/ui/src/skins/nebula/nebula.css'),'utf8');
const instrumentCss=readFileSync(resolve('../../packages/ui/src/skins/instrument/instrument.css'),'utf8');
let cleanup=()=>{};
beforeEach(()=>{localStorage.clear();contextPresence.count=0;});
afterEach(()=>{cleanup();document.documentElement.removeAttribute('data-skin');});
it('uses a temporary narrow-screen toggle without overwriting the wide preference',async()=>{
 localStorage.setItem('nyabula.contextOpen','1');const viewport=reactive({width:1600});const inDevice=ref(true);let column!:ReturnType<typeof useContextColumn>;
 const host=document.createElement('div');document.body.append(host);const app=createApp({setup(){column=useContextColumn(inDevice);return()=>h('div');}});app.provide('formFactor',{viewport});app.mount(host);cleanup=()=>{app.unmount();host.remove();};
 contextPresence.count=1;expect(column.show.value).toBe(true);
 viewport.width=1024;await nextTick();expect(column.show.value).toBe(false);
 column.toggle();expect(column.show.value).toBe(true);expect(localStorage.getItem('nyabula.contextOpen')).toBe('1');
 inDevice.value=false;expect(column.show.value).toBe(false);inDevice.value=true;contextPresence.count=0;expect(column.show.value).toBe(false);
 contextPresence.count=1;viewport.width=1600;await nextTick();column.toggle();expect(column.show.value).toBe(false);expect(localStorage.getItem('nyabula.contextOpen')).toBe('0');
});
it('limits raw field styling to inputs, not MdTextField wrapper labels',()=>{
 const wrapper=document.createElement('label');wrapper.className='tf field';wrapper.innerHTML='<span class="tf-box"><input type="password"></span>';
 const native=document.createElement('input');native.className='field';document.body.append(wrapper,native);cleanup=()=>{wrapper.remove();native.remove();};
 for(const [skin,css] of [['classic',baseCss],['nebula',nebulaCss],['instrument',instrumentCss]]){
  document.documentElement.dataset.skin=skin;
  const selector=css.split('\n').find(line=>line.includes("number")&&line.includes('input.field')&&line.includes('{'))!.split('{')[0].trim();
  // happy-dom 15 does not implement descendant selectors inside :where().
  // Verify the combined rule in Chromium; check its two parts here.
  const exclusion = ':not(:where(.tf-box *))';
  expect(selector.endsWith(exclusion)).toBe(true);
  const controls = selector.slice(0, -exclusion.length);
  expect(wrapper.matches(controls)).toBe(false);
  expect(native.matches(controls)).toBe(true);
  expect(wrapper.querySelector('input')!.matches('.tf-box *')).toBe(true);
 }
});

it('keeps the legacy Nebula text-input skin off the inner MdTextField input', () => {
 const rules = nebulaCss.split('\n').filter(line => line.includes("input[type='text']"));
 expect(rules.length).toBeGreaterThan(0);
 for (const rule of rules) expect(rule).toContain("input[type='text']:not(:where(.tf-box *))");
});
it('puts collapsed execution details before the model reply', () => {
 const source = readFileSync(resolve('src/views/agent/ChatThread.vue'), 'utf8');
 expect(source).toContain('<summary>设备执行记录</summary>');
 expect(source.indexOf('<details v-if="m.outcome')).toBeLessThan(source.indexOf('<MarkdownText'));
 expect(source).not.toMatch(/<details[^>]*\sopen(?:[\s=>])/);
});
