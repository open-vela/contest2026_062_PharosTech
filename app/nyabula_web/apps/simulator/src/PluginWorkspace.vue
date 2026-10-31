<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { NyaUiSurface, type NyaUiCommand, type NyaUiTree } from '@nyabula/nyaui-vue';
import { permissionZh, MdSwitch } from '@nyabula/ui';
import { statusLabel, useWorkbench } from './workbench';
const w=useWorkbench();
const path=ref('');const view=ref<'component'|'html'>('component');const json=ref(false);const narrow=ref(false);
const tree=ref<NyaUiTree|null>(null);const treeError=ref('');const details=ref<{path:string}|null>(null);let sequence=0;
const available=computed(()=>tree.value?.views?Object.keys(tree.value.views):tree.value?[typeof tree.value.html==='string'?'html':'component']:[]);
const logs=computed(()=>(w.snapshot.value?.logs??[]).filter(l=>l.pluginId===w.selected.value).slice(-4));
async function loadTree(){const id=w.selected.value;const current=++sequence;if(!id){tree.value=null;return;}try{const value=await w.call('ui_tree',{pluginId:id,role:w.role.value});if(current===sequence){tree.value=value;treeError.value='';if(!available.value.includes(view.value))view.value=available.value[0] as 'component'|'html'??'component';}}catch(e){if(current===sequence){tree.value=null;treeError.value=e instanceof Error?e.message:String(e);}}}
watch(()=>[w.selected.value,w.snapshot.value?.revision,w.role.value],()=>void loadTree());
watch(w.selected,async id=>{view.value='component';json.value=false;details.value=id?await w.call('plugin_inspect',{pluginId:id}).catch(()=>null):null;},{immediate:true});
async function load(){const value=await w.act('plugin_load',{path:path.value},'源码已加载，权限默认关闭');if(value){w.selected.value=value.id;path.value='';}}
async function event(command:NyaUiCommand){await w.act('ui_event',{pluginId:w.selected.value,role:w.role.value,componentId:command.componentId,event:command.event,values:command.values,view:command.view},'插件事件已执行');await loadTree();}
async function grantAll(){for(const p of w.plugin.value?.permissions??[]){if(!p.granted)await w.act('plugin_grant',{pluginId:w.selected.value,permission:p.name},'已授予声明的权限');}}
</script>
<template>
  <div class="plugin-workspace">
    <form class="load-bar" @submit.prevent="load"><label for="source-path">本地源码目录</label><input id="source-path" v-model="path" placeholder="C:\projects\my-plugin"/><button class="primary" :disabled="!path.trim()||w.busy.value">加载源码</button></form>
    <div class="debug-layout">
      <aside class="plugin-list panel"><div class="panel-heading"><h2>插件</h2><span class="muted">{{w.plugins.value.length}}</span></div>
        <div v-for="p in w.plugins.value" :key="p.id" class="plugin-item" :class="{selected:p.id===w.selected.value}"><button class="plugin-item-open" :title="p.id" @click="w.selected.value=p.id"><strong>{{p.name}}</strong><small>{{p.version}}</small></button><MdSwitch :model-value="p.state==='running'" :disabled="w.busy.value" :aria-label="`${p.state==='running'?'停止':'启动'}${p.name}`" @update:model-value="w.act(p.state==='running'?'plugin_stop':'plugin_start',{pluginId:p.id},'插件状态已更新')"/></div>
        <p v-if="!w.plugins.value.length" class="empty-copy">载入一个含 manifest.json 的插件目录，开始调试。</p>
      </aside>
      <section class="preview-panel panel">
        <header class="panel-heading preview-heading"><div><h2>{{w.plugin.value?.name??'插件预览'}}</h2><span class="muted">{{w.plugin.value?statusLabel(w.plugin.value.state):'请选择插件'}}<span v-if="w.plugin.value"> · 第 {{w.plugin.value.generation}} 代</span></span></div><div class="actions" v-if="w.plugin.value"><button :disabled="w.busy.value" @click="w.act(w.plugin.value.state==='running'?'plugin_stop':'plugin_start',{pluginId:w.selected.value},w.plugin.value.state==='running'?'插件已停止':'插件已启动')">{{w.plugin.value.state==='running'?'停止':'启动'}}</button><button :disabled="w.busy.value" @click="w.act('plugin_reload',{pluginId:w.selected.value},'源码已重载')">重载源码 ↻</button></div></header>
        <div class="preview-toolbar"><div class="segments" role="tablist" aria-label="预览显示方式"><button role="tab" :aria-selected="!json&&view==='component'" :disabled="!available.includes('component')" :class="{active:!json&&view==='component'}" @click="view='component';json=false">组件 UI</button><button role="tab" :aria-selected="!json&&view==='html'" :disabled="!available.includes('html')" :class="{active:!json&&view==='html'}" @click="view='html';json=false">HTML UI</button><button role="tab" :aria-selected="json" :class="{active:json}" @click="json=true">结构 JSON</button></div><div class="preview-options"><label>测试身份<select v-model="w.role.value"><option value="owner">主人</option><option value="family">家人</option><option value="guest">访客</option></select></label><button class="icon-button" :class="{active:narrow}" title="切换窄屏预览" @click="narrow=!narrow">▯</button></div></div>
        <div class="preview-canvas" :class="{narrow}">
          <pre v-if="json" class="json-code">{{JSON.stringify(tree,null,2)}}</pre>
          <NyaUiSurface v-else-if="tree" :tree="tree" :view="view" :plugin-id="w.selected.value" :show-switcher="false" :busy="w.busy.value" @command="event"/>
          <div v-else class="preview-empty"><span class="empty-symbol">{ }</span><h3>{{w.plugin.value?.state==='running'?'等待插件发布页面':'插件尚未运行'}}</h3><p>{{w.plugin.value?.state==='running'?treeError:'在右侧检查能力授权，再启动插件。'}}</p><button v-if="w.plugin.value?.state!=='running' && w.plugin.value" class="primary" @click="w.act('plugin_start',{pluginId:w.selected.value},'插件已启动')">启动插件</button></div>
        </div>
        <div class="mini-console"><div class="console-heading"><span>运行日志</span><button class="text-button" @click="w.section.value='logs'">查看全部 →</button></div><div v-for="log in logs" :key="log.sequence" class="console-line"><time>{{new Date(log.time).toLocaleTimeString('zh-CN',{hour12:false})}}</time><span>{{log.message}}</span></div><p v-if="!logs.length" class="muted">尚无插件输出</p></div>
      </section>
      <aside class="inspector panel"><div class="panel-heading"><h2>运行时检查</h2></div><template v-if="w.plugin.value"><dl class="facts"><dt>引擎</dt><dd>Goja / JavaScript</dd><dt>版本</dt><dd>{{w.plugin.value.version}}</dd><dt>事件超时</dt><dd>{{w.snapshot.value?.runtime.eventDeadlineMs}} ms</dd><dt>源码</dt><dd class="source-path">{{details?.path}}</dd></dl><div class="inspector-heading"><h3>能力权限</h3><button class="text-button" :disabled="w.busy.value" @click="grantAll">全部授予</button></div><label v-for="p in w.plugin.value.permissions" :key="p.name" class="permission"><span><strong>{{permissionZh(p.name)}}</strong><code>{{p.name}}</code></span><input type="checkbox" :checked="p.granted" :aria-label="permissionZh(p.name)" :disabled="w.busy.value" @change="w.act(p.granted?'plugin_revoke':'plugin_grant',{pluginId:w.selected.value,permission:p.name},'权限已更新')"/></label><div v-if="w.plugin.value.lastError" class="runtime-error"><h3>最近错误</h3><pre>{{w.plugin.value.lastError}}</pre></div><button class="wide" @click="w.section.value='storage'">查看与编辑私有数据 →</button></template><p v-else class="empty-copy">选择插件后查看运行状态与能力授权。</p></aside>
    </div>
  </div>
</template>
