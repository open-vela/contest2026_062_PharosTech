<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useWorkbench } from './workbench';
const w=useWorkbench();const storage=ref<Record<string,string>>({});const key=ref('');const value=ref('');let ticket=0;
async function read(){const id=w.selected.value;const sequence=++ticket;if(!id){storage.value={};return;}try{const result=await w.call('plugin_inspect',{pluginId:id});if(ticket===sequence)storage.value=result.storage;}catch{if(ticket===sequence)storage.value={};}}
watch(()=>[w.selected.value,w.snapshot.value?.revision],()=>void read(),{immediate:true});
const size=computed(()=>new TextEncoder().encode(JSON.stringify(storage.value)).length);
async function save(){const result=await w.act('storage_set',{pluginId:w.selected.value,key:key.value,value:value.value},'私有数据已写入');if(result)await read();}
async function remove(name:string){if(window.confirm(`删除测试数据「${name}」？`)){await w.act('storage_set',{pluginId:w.selected.value,key:name,value:null},'数据已删除');await read();}}
</script>
<template>
  <div class="page-content"><div class="section-toolbar"><label>插件<select v-model="w.selected.value"><option v-for="plugin in w.plugins.value" :key="plugin.id" :value="plugin.id">{{plugin.name}}</option></select></label><div class="actions"><span class="chip">{{Object.keys(storage).length}} 个键</span><span class="chip mono">{{size}} B</span><button @click="read">刷新</button></div></div><div class="two-columns"><section class="panel"><div class="panel-heading"><h2>当前私有数据</h2></div><div v-for="(data,name) in storage" :key="name" class="storage-row"><button @click="key=String(name);value=data"><code>{{name}}</code><pre>{{data}}</pre></button><button class="text-button danger-text" @click="remove(String(name))">删除</button></div><p v-if="!Object.keys(storage).length" class="empty-copy">这个插件尚未保存数据。</p></section><form class="panel" @submit.prevent="save"><div class="panel-heading"><h2>编辑测试数据</h2></div><div class="form-body"><label>键<input v-model="key" required maxlength="128" placeholder="count"/></label><label>值 · 字符串<textarea v-model="value" rows="9" class="code-editor" maxlength="4096" spellcheck="false"/></label><p class="field-hint">直接修改插件私有存储。需要插件重新读取数据时，重载源码即可。</p><button class="primary" :disabled="w.busy.value||!w.selected.value||!key">写入数据</button></div></form></div></div>
</template>
