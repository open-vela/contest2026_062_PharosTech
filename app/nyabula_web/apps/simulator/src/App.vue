<script setup lang="ts">
import { onBeforeUnmount, onMounted } from 'vue';
import { NyabulaLogo, UiIcon } from '@nyabula/ui';
import { useWorkbench } from './workbench';
import PluginWorkspace from './PluginWorkspace.vue';
import DevicePanel from './DevicePanel.vue';
import FixturesPanel from './FixturesPanel.vue';
import StoragePanel from './StoragePanel.vue';
import LogsPanel from './LogsPanel.vue';
const w=useWorkbench();
const sections=[['plugins','插件调试','extension'],['device','设备与配对','devices'],['fixtures','模拟与故障','tune'],['storage','私有存储','storage'],['logs','日志与协议','terminal']];
let timer:ReturnType<typeof setInterval>;
onMounted(()=>{void w.refresh();timer=setInterval(()=>void w.refresh(),900);});
onBeforeUnmount(()=>clearInterval(timer));
</script>
<template>
  <div class="workbench">
    <aside class="sidebar">
      <div class="brand"><NyabulaLogo :size="36"/><div><strong>Nyabula</strong><span>SIMULATOR / 工作台</span></div></div>
      <div class="runtime-badge"><span class="dot" :class="{offline:!w.connected.value}"/>{{ w.connected.value?'本机运行中':'正在连接' }}<code>GOJA</code></div>
      <nav><button v-for="[id,label,icon] in sections" :key="id" :class="{active:w.section.value===id}" @click="w.section.value=id"><UiIcon :name="icon" :size="20"/>{{label}}</button></nav>
      <div class="sidebar-bottom">
        <span class="eyebrow">当前配对码</span><button class="pair-code" @click="w.section.value='device'">{{w.snapshot.value?.device.pairCode??'------'}}</button>
        <small>在产品 WebUI 中使用；工作台无需配对。</small>
        <a class="preview-link" href="/connect" target="_blank" rel="noopener">打开产品 WebUI 仿真 <span>↗</span></a>
      </div>
    </aside>
    <main class="main">
      <header class="topbar"><div><span class="eyebrow">开发控制台</span><h1>{{sections.find(s=>s[0]===w.section.value)?.[1]}}</h1></div><div class="top-status"><span class="chip">{{w.plugins.value.length}} 个插件</span><span class="chip">{{w.snapshot.value?.device.clients.length??0}} 个客户端</span><span class="chip mono">MCP {{w.snapshot.value?.tools.length??'—'}} 工具</span></div></header>
      <div v-if="!w.connected.value" class="banner warn">{{w.snapshot.value?'连接中断，保留最后一次状态。正在重新连接…':'正在连接本机模拟器…'}}</div>
      <div v-if="w.failure.value" class="banner error" role="alert"><strong>操作未完成</strong><span>{{w.failure.value}}</span><button @click="w.failure.value=''">关闭</button></div>
      <div v-else-if="w.notice.value" class="banner success" role="status">{{w.notice.value}}<button @click="w.notice.value=''">关闭</button></div>
      <PluginWorkspace v-if="w.section.value==='plugins'"/>
      <DevicePanel v-else-if="w.section.value==='device' && w.snapshot.value"/>
      <FixturesPanel v-else-if="w.section.value==='fixtures' && w.snapshot.value"/>
      <StoragePanel v-else-if="w.section.value==='storage'"/>
      <LogsPanel v-else-if="w.section.value==='logs'"/>
      <footer><span>只控制本机模拟器 · 不连接真实硬件</span><code>{{w.snapshot.value?.device.stateFile}}</code></footer>
    </main>
  </div>
</template>
