<script setup lang="ts">
import { reactive, ref } from 'vue';
import { EyeCanvas } from '@nyabula/ui';
import { useWorkbench, statusLabel } from './workbench';
const modeLabel=(mode:string)=>({idle:'待机',curious:'好奇',happy:'开心',processing:'思考',star:'星星眼',heart:'爱心眼',sleepy:'困倦',sleep:'睡眠',angry:'生气',sad:'难过',surprise:'惊讶',dizzy:'晕眩',derp:'呆萌'}[mode]??mode);
const w=useWorkbench();
const clone = <T,>(value:T):T => JSON.parse(JSON.stringify(value));
const source=w.snapshot.value!.device;
const initialUptime=ref(source.uptimeSeconds);
const draft=reactive({...clone(source.config),uptimeSeconds:source.uptimeSeconds,deviceId:source.deviceId,pairCode:source.pairCode,claimCode:source.claimCode,cloud:clone(source.cloud)});
const eyeJSON=ref(JSON.stringify(source.eye,null,2));
function refreshDraft(){const device=w.snapshot.value!.device;Object.assign(draft,clone(device.config),{deviceId:device.deviceId,pairCode:device.pairCode,claimCode:device.claimCode,cloud:clone(device.cloud),uptimeSeconds:device.uptimeSeconds});initialUptime.value=device.uptimeSeconds;eyeJSON.value=JSON.stringify(device.eye,null,2);}
async function save(){const result=await w.act('simulator_configure',{data:{name:draft.name,deviceId:draft.deviceId,pairCode:draft.pairCode,claimCode:draft.claimCode,pairingEnabled:draft.pairingEnabled,battery:draft.battery,wifi:draft.wifi,...(draft.uptimeSeconds!==initialUptime.value?{uptimeSeconds:draft.uptimeSeconds}:{}),demo:draft.demo,cloud:draft.cloud}},'模拟器配置已保存并立即生效');if(result)initialUptime.value=draft.uptimeSeconds;}
async function applyEye(){try{await w.act('simulator_configure',{data:{eye:JSON.parse(eyeJSON.value)}},'眼睛运行态已更新');}catch{w.failure.value='眼睛状态不是有效 JSON';}}
async function resetPairing(){if(window.confirm('清除本机模拟器的所有配对和成员？产品 WebUI 需要重新配对。'))await w.act('simulator_reset_pairing',{},'模拟器配对已重置');}
</script>
<template>
  <div class="page-content device-grid">
    <form class="panel config-panel" @submit.prevent="save">
      <div class="panel-heading"><h2>设备配置</h2><button type="button" class="text-button" @click="refreshDraft">从运行态读取</button></div>
      <div class="form-grid"><label>设备名称<input v-model="draft.name" required maxlength="64"/></label><label>设备 ID<input v-model="draft.deviceId" required maxlength="63" class="mono"/></label></div>
      <h3 class="section-heading">配对</h3><div class="form-grid"><label>六位配对码<input v-model="draft.pairCode" class="code-input" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" required/></label><label>云端认领码<input v-model="draft.claimCode" class="code-input" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" required/></label></div><label class="check-row"><input v-model="draft.pairingEnabled" type="checkbox"/>允许新客户端配对</label><p class="field-hint">修改配对码不影响已经配对的客户端。</p>
      <h3 class="section-heading">电量与网络</h3><div class="form-grid"><label>电量 · %<input v-model.number="draft.battery.level" type="number" min="0" max="100"/></label><label>信号 · dBm<input v-model.number="draft.wifi.rssi" type="number" min="-127" max="0"/></label><label>WiFi 名称<input v-model="draft.wifi.ssid" maxlength="32"/></label><label>启动时长 · 秒<input v-model.number="draft.uptimeSeconds" type="number" min="0"/></label></div><div class="actions"><label class="check-row"><input v-model="draft.battery.charging" type="checkbox"/>充电中</label><label class="check-row"><input v-model="draft.wifi.connected" type="checkbox"/>WiFi 已连接</label><label class="check-row"><input v-model="draft.demo" type="checkbox"/>表情轮播</label></div>
      <details class="advanced"><summary>云配置夹具</summary><label>Cloud 地址<input v-model="draft.cloud.url" placeholder="ws://127.0.0.1:8088"/></label><label class="check-row"><input v-model="draft.cloud.enabled" type="checkbox"/>模拟已启用</label><p class="field-hint">此开发实例不启动外部 Cloud 连接；这些值用于配置状态测试。</p></details>
      <div class="form-actions"><button class="primary" :disabled="w.busy.value">保存配置</button><button type="button" class="danger-text" @click="resetPairing">重置配对与成员</button></div>
    </form>
    <div class="stack">
      <section class="panel"><div class="panel-heading"><h2>设备显示预览</h2><span class="chip">{{w.snapshot.value?.device.eye.expression?.mode}}</span></div><div class="eye-preview"><EyeCanvas :eye-state="w.snapshot.value?.device.eye" @look="w.call('device_request',{role:'owner',topic:'eye.look',data:$event})"/></div><div class="mode-grid"><button v-for="mode in w.snapshot.value?.device.modes" :key="mode" :class="{active:w.snapshot.value?.device.eye.expression?.mode===mode}" @click="w.act('device_request',{role:'owner',topic:'eye.mode',data:{mode}},'显示状态已更新')">{{modeLabel(mode)}}</button></div></section>
      <section class="panel"><div class="panel-heading"><h2>连接与身份</h2><span class="chip">{{w.snapshot.value?.device.pairedTokens}} 已配对</span></div><div class="client-row" v-for="client in w.snapshot.value?.device.clients" :key="client.id"><span class="dot" :class="{offline:!client.authenticated}"/><span><strong>{{client.name}}</strong><small>连接 #{{client.id}}</small></span><span class="chip">{{client.role?statusLabel(client.role):'待配对'}}</span></div><p v-if="!w.snapshot.value?.device.clients.length" class="empty-copy">暂无产品客户端连接。调试工作台不占用配对连接。</p></section>
      <section class="panel"><div class="panel-heading"><h2>完整眼睛状态</h2><button class="text-button" @click="eyeJSON=JSON.stringify(w.snapshot.value?.device.eye,null,2)">读取最新</button></div><textarea v-model="eyeJSON" class="code-editor" rows="10" spellcheck="false" aria-label="眼睛状态 JSON"/><div class="form-actions"><button :disabled="w.busy.value" @click="applyEye">应用眼睛状态</button></div></section>
    </div>
  </div>
</template>
