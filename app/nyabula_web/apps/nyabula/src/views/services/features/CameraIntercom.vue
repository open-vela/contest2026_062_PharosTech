<script setup lang="ts">
import { computed } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { useIntercom } from '../../../composables/useIntercom';
const call = useIntercom();
const { state, active, held, pending, releasing, error, canStart, microphoneSupported, label } = call;
const level = computed(() => Math.min(100, Math.sqrt((state.value?.level ?? 0) / 32768) * 100));
function down(event: PointerEvent) {
  if (!event.isPrimary || event.button !== 0) return;
  (event.currentTarget as HTMLElement).setPointerCapture(event.pointerId);
  void call.press();
}
function keyDown(event: KeyboardEvent) { if (!event.repeat) void call.press(); }
</script>
<template>
  <section class="camera-intercom ui-panel">
    <div class="intercom-title"><div><h2>远程对讲</h2><p role="status">{{ label }}</p></div><UiIcon name="mic" :size="26" /></div>
    <div class="intercom-actions">
      <MdButton v-if="!active" :disabled="!canStart" @click="call.start">开启对讲</MdButton>
      <template v-else>
        <button type="button" class="push-to-talk" :class="{ held }" :disabled="!microphoneSupported || releasing || state?.phase === 'starting'"
          @pointerdown.prevent="down" @pointerup="call.release" @pointercancel="call.release" @lostpointercapture="call.release"
          @keydown.space.prevent="keyDown" @keyup.space.prevent="call.release" @keydown.enter.prevent="keyDown" @keyup.enter.prevent="call.release" @blur="call.release">
          <UiIcon name="mic" :size="20" />{{ pending ? '连接麦克风…' : held ? '正在说话 · 松开结束' : releasing ? '送出尾音…' : '按住说话' }}
        </button>
        <MdButton variant="outlined" @click="call.stop">结束对讲</MdButton>
        <div class="intercom-meter" role="meter" aria-label="设备收音电平" :aria-valuenow="Math.round(level)" :aria-valuemin="0" :aria-valuemax="100"><i :style="{ width: `${held ? 0 : level}%` }" /></div>
      </template>
    </div>
    <p v-if="error" class="intercom-error" role="alert">{{ error }}</p>
    <p class="intercom-hint">按住说话，松开收听设备；对讲期间语音唤醒暂时让出麦克风，结束后恢复。</p>
    <p v-if="active && !microphoneSupported" class="intercom-hint">当前连接可收听。使用 HTTPS 远程页面后，浏览器才能申请麦克风权限。</p>
  </section>
</template>
<style scoped>
.camera-intercom{padding:20px;display:grid;gap:16px}.intercom-title{display:flex;align-items:center;justify-content:space-between;gap:16px}.intercom-title h2{margin:0;font-size:18px}.intercom-title p{margin:6px 0 0;font-size:13px;color:var(--md-on-surface-variant)}.intercom-title>.ui-icon{color:var(--md-primary)}.intercom-actions{display:flex;align-items:center;gap:12px;flex-wrap:wrap}.push-to-talk{display:inline-flex;align-items:center;justify-content:center;gap:9px;min-width:164px;min-height:48px;padding:12px 18px;border:1px solid var(--md-primary);border-radius:var(--radius-m);background:var(--md-primary-container);color:var(--md-on-primary-container);font:600 14px var(--font-body);touch-action:none;user-select:none;cursor:pointer}.push-to-talk.held{background:var(--md-primary);color:var(--md-on-primary)}.push-to-talk:disabled{opacity:.45;cursor:default}.push-to-talk:focus-visible{outline:2px solid var(--md-primary);outline-offset:3px}.intercom-meter{height:7px;min-width:100px;max-width:240px;flex:1;background:var(--md-surface-container-highest);overflow:hidden;border-radius:8px}.intercom-meter i{display:block;height:100%;background:var(--md-primary);transition:width .1s}.intercom-hint{margin:0;font-size:13px;line-height:1.7;color:var(--md-on-surface-variant)}.intercom-error{margin:0;color:var(--md-error);font-size:14px;line-height:1.6}
@media(max-width:600px){.camera-intercom{padding:16px}.intercom-meter{flex-basis:100%;max-width:none}}
</style>
