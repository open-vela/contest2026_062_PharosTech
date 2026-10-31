<script setup lang="ts">
import { ref } from 'vue';
import { permissionZh } from '@nyabula/ui';
import { useWorkbench } from './workbench';
const w=useWorkbench();const key=ref('ai.response');const value=ref('"你好，今天也一起度过吧。"');const capability=ref('network.request');const reason=ref('模拟网络不可达');
const capabilities=['core.log','storage.read','storage.write','network.request','ui.notify','ui.page','ui.html','ai.invoke'];
async function saveMock(){try{const parsed=JSON.parse(value.value);await w.act('mock_set',{key:key.value,value:parsed},'模拟响应已更新');}catch{w.failure.value='响应内容必须是有效 JSON；文本请用双引号包裹。';}}
function preset(kind:string){key.value=kind==='ai'?'ai.response':'network:https://example.test/data';value.value=JSON.stringify(kind==='ai'?'你好，今天也一起度过吧。':{status:200,body:JSON.stringify({ok:true})},null,2);}
function edit(name:string,data:unknown){key.value=name;value.value=JSON.stringify(data,null,2);}
</script>
<template>
  <div class="page-content two-columns">
    <section class="panel"><div class="panel-heading"><h2>模拟响应</h2><span class="chip">确定性夹具</span></div><div class="form-body"><label>响应键<input v-model="key" placeholder="network:https://example.test/data"/></label><div class="quick-picks"><button @click="preset('ai')">AI 文本</button><button @click="preset('network')">HTTP 响应</button></div><label>返回值 · JSON<textarea v-model="value" rows="8" class="code-editor" spellcheck="false"/></label><button class="primary" :disabled="w.busy.value||!key" @click="saveMock">应用模拟响应</button></div><div class="fixture-list"><button v-for="(data,name) in w.snapshot.value?.fixtures.mocks" :key="name" class="fixture-row" @click="edit(String(name),data)"><code>{{name}}</code><span>{{JSON.stringify(data)}}</span><span>编辑 →</span></button><p v-if="!Object.keys(w.snapshot.value?.fixtures.mocks??{}).length" class="empty-copy">尚未配置响应。未配置的网络与 AI 调用会明确报错。</p></div></section>
    <section class="panel"><div class="panel-heading"><h2>故障注入</h2><span class="chip amber">影响后续能力调用</span></div><div class="form-body"><label>目标能力<select v-model="capability"><option v-for="item in capabilities" :key="item" :value="item">{{permissionZh(item)}} · {{item}}</option></select></label><label>错误内容<input v-model="reason" placeholder="例如：模拟磁盘已满"/></label><button class="danger" :disabled="w.busy.value||!reason" @click="w.act('fault_set',{key:capability,value:reason},'故障已注入')">注入故障</button></div><div class="fixture-list"><div v-for="(message,name) in w.snapshot.value?.fixtures.faults" :key="name" class="fault-row"><div><strong>{{permissionZh(String(name))}}</strong><p>{{message}}</p></div><button @click="w.act('fault_set',{key:name,value:null},'故障已清除')">清除</button></div><p v-if="!Object.keys(w.snapshot.value?.fixtures.faults??{}).length" class="empty-copy">当前没有注入故障。</p></div></section>
  </div>
</template>
