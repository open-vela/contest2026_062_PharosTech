import { expect, it } from 'vitest';
import source from '../src/skins/nebula/home/NebulaHome.vue?raw';
import { parse, compileStyle } from 'vue/compiler-sfc';
it('hides only the halo during handoff, never the document root',()=>{
 const {descriptor}=parse(source);
 const css=compileStyle({source:descriptor.styles[0].content,filename:'NebulaHome.vue',id:'halo-test',scoped:true});
 expect(css.errors).toEqual([]);
 expect(css.code).toMatch(/html\.nb-eyes-handoff \.nb-home \.halo\s*\{\s*opacity:\s*0/);
 expect(css.code).not.toMatch(/html\.nb-eyes-handoff\s*\{/);
 expect(css.code).toContain('nb-halo-arrive');
});
