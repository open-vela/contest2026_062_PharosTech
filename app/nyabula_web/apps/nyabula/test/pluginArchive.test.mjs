import test from 'node:test';
import assert from 'node:assert/strict';
import { zipSync, strToU8 } from 'fflate';
import { readPluginArchive, uploadPluginArchive } from '../src/lib/pluginArchive.ts';
const files = {'manifest.json':strToU8(JSON.stringify({id:'test',name:'中文插件',version:'1.0.0'})), 'main.js':new Uint8Array(15000)};
test('reads Chinese metadata and rejects traversal/oversize',()=>{
 assert.equal(readPluginArchive(zipSync(files)).name,'中文插件');
 assert.throws(()=>readPluginArchive(zipSync({'../outside':new Uint8Array(1)})),/不安全/);
 assert.throws(()=>readPluginArchive(zipSync({...files,huge:new Uint8Array(9*1024*1024)})),/解压后/);
 assert.throws(()=>readPluginArchive(zipSync({'x.js':new Uint8Array(0)})),/manifest/);
});
test('upload splits chunks, commits, and cancels on failure',async()=>{
 const calls=[];let progress=0;
 await uploadPluginArchive(readPluginArchive(zipSync(files)),async(t,d)=>{calls.push([t,d]);return {uploadId:'test'}},n=>progress=n);
 assert.equal(calls.at(-1)[0],'plugin.upload.commit');assert.equal(progress,100);
 assert.deepEqual(calls.filter(c=>c[1].path==='main.js').map(c=>c[1].offset),[0,6144,12288]);
 const failed=[];
 await assert.rejects(uploadPluginArchive(readPluginArchive(zipSync(files)),async(t,d)=>{failed.push(t);if(t.endsWith('chunk'))throw Error('offline');return {uploadId:'test'}},()=>{}),/offline/);
 assert.equal(failed.at(-1),'plugin.upload.cancel');
});
