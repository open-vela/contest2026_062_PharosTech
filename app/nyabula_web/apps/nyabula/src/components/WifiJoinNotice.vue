<script setup lang="ts">
/* Shown after the device accepted new WiFi credentials. Losing the link at
 * this point is the expected outcome, so this explains what happens next. */
import { UiIcon } from '@nyabula/ui';

defineProps<{ ssid: string; viaHotspot: boolean; linkDropped: boolean }>();
</script>

<template>
  <div class="join">
    <div class="join-head">
      <span class="orb"><UiIcon name="sync" :size="26" class="spin" /></span>
      <div>
        <h3 class="join-title">设备正在加入「{{ ssid }}」</h3>
        <p class="muted join-sub">{{ linkDropped ? '与设备的连接已断开，这是正常现象。' : '通常需要十几秒。' }}</p>
      </div>
    </div>
    <ol class="steps">
      <template v-if="viaHotspot">
        <li>设备的配网热点马上会关闭，所以这个页面会连不上设备，不用担心。</li>
        <li>请把手机重新连回家里的 WiFi「{{ ssid }}」。</li>
      </template>
      <li v-else>设备切换网络期间，这个页面可能会暂时连不上设备。</li>
      <li>设备连上以后，它的眼睛屏幕会显示新的地址，在浏览器里打开那个地址就能继续使用。</li>
    </ol>
    <p class="muted fallback">如果过了一两分钟，设备的眼睛又显示出配网二维码，说明它没能连上（密码不对，或者离路由器太远）。重新扫描那个二维码，再试一次即可。</p>
  </div>
</template>

<style scoped>
.join { display: flex; flex-direction: column; gap: 14px; }
.join-head { display: flex; align-items: center; gap: 14px; }
.orb { width: 48px; height: 48px; border-radius: 50%; display: grid; place-items: center; flex: none; background: rgba(var(--md-primary-rgb), 0.14); color: var(--md-primary); }
.join-title { margin: 0; font: 600 17px var(--font-title); color: var(--md-on-surface); word-break: break-all; }
.join-sub { margin: 2px 0 0; font-size: 13px; }
.steps { margin: 0; padding-left: 22px; display: flex; flex-direction: column; gap: 8px; font-size: 14.5px; line-height: 1.55; color: var(--md-on-surface); }
.fallback { margin: 0; font-size: 13px; line-height: 1.55; }
.spin { animation: join-spin 1.6s linear infinite; }
@keyframes join-spin { to { transform: rotate(360deg); } }
</style>
