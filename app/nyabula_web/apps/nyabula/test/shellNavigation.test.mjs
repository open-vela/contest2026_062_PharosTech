import test from 'node:test';
import assert from 'node:assert/strict';
import {hasAppBack} from '../src/lib/shellNavigation.ts';
function router(back,target={name:'services',matched:[{}],meta:{}}){return {options:{history:{state:{back}}},resolve:()=>target};}
test('never return to blank, foreign, or the same page',()=>{
 for(const back of [null,undefined,'','about:blank','https://example.com','//example.com','/current'])assert.equal(hasAppBack(router(back),'/current'),false);
});
test('only renderable app routes qualify; access and catch-all routes do not',()=>{
 assert.equal(hasAppBack(router('/features'),'/current'),true);
 for(const name of ['connect','login','setup-password','provision',undefined])assert.equal(hasAppBack(router('/previous',{name,matched:[{}],meta:{}}),'/current'),false);
 assert.equal(hasAppBack(router('/previous',{name:'unknown',matched:[],meta:{}}),'/current'),false);
 assert.equal(hasAppBack(router('/previous',{name:'gate',matched:[{}],meta:{bare:true}}),'/current'),false);
});
