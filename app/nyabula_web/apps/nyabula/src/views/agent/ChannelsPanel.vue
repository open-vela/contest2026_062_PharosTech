<script setup lang="ts">
import { ref } from 'vue';
import MqttPanel from './MqttPanel.vue';
import WeixinPanel from './WeixinPanel.vue';
import FeishuPanel from './FeishuPanel.vue';
const selected = ref('mqtt');
</script>
<template>
  <section class="channels-panel">
    <nav class="channel-tabs" aria-label="消息渠道类型">
      <button :aria-current="selected === 'mqtt' ? 'page' : undefined" @click="selected = 'mqtt'">MQTT</button>
      <button :aria-current="selected === 'weixin' ? 'page' : undefined" @click="selected = 'weixin'">微信</button>
      <button :aria-current="selected === 'feishu' ? 'page' : undefined" @click="selected = 'feishu'">飞书</button>
    </nav>
    <MqttPanel v-if="selected === 'mqtt'" />
    <WeixinPanel v-else-if="selected === 'weixin'" />
    <FeishuPanel v-else />
  </section>
</template>
<style scoped>
.channel-tabs { display: flex; gap: 8px; max-width: 760px; margin: 0 auto 20px; }
button { min-height: 44px; padding: 8px 20px; border-radius: 22px; border: 1px solid var(--md-outline-variant); color: var(--md-on-surface); background: var(--md-surface); font: inherit; cursor: pointer; }
button[aria-current] { color: var(--md-on-secondary-container); background: var(--md-secondary-container); }
</style>
