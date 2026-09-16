<script setup lang="ts">
/* Connect page: users type only an IP (plus optional port); the ws URL is
 * assembled as ws://<ip>:<port>/nyalink. An "advanced" fold still accepts a
 * full address for non-default paths/schemes. */
import { ref, computed } from 'vue';
import { useLinkStore } from '../stores/link';
import { MdButton, MdCard } from '@nyabula/ui';

const DEFAULT_PORT = 7788;

const link = useLinkStore();

/* Prefill ip/port from the persisted URL when it matches the simple form. */
const m = /^ws:\/\/([^/:]+)(?::(\d+))?\/nyalink$/.exec(link.url);
const ip = ref(m?.[1] ?? 'localhost');
const port = ref(m?.[2] ?? '');
const showAdvanced = ref(false);
const fullAddr = ref(link.url);

const busy = computed(() =>
  ['connecting', 'authenticating', 'reconnecting'].includes(link.state),
);

const builtUrl = computed(() => {
  const host = ip.value.trim() || 'localhost';
  const p = port.value.trim() || String(DEFAULT_PORT);
  return `ws://${host}:${p}/nyalink`;
});

function doConnect() {
  link.connect(showAdvanced.value ? fullAddr.value.trim() : builtUrl.value);
}
</script>

<template>
  <div class="connect-wrap">
    <h1 class="brand">Nyabula</h1>
    <p class="sub">局域网控制面板</p>
    <MdCard class="card">
      <template v-if="!showAdvanced">
        <div class="addr-row">
          <div class="field grow">
            <label class="lbl">设备 IP</label>
            <input v-model="ip" type="text" spellcheck="false" placeholder="localhost" @keyup.enter="doConnect" />
          </div>
          <div class="field port">
            <label class="lbl">端口</label>
            <input v-model="port" type="text" inputmode="numeric" spellcheck="false" placeholder="7788" @keyup.enter="doConnect" />
          </div>
        </div>
        <p class="preview">{{ builtUrl }}</p>
      </template>
      <template v-else>
        <label class="lbl">完整地址</label>
        <input v-model="fullAddr" type="text" spellcheck="false" placeholder="ws://localhost:7788/nyalink" @keyup.enter="doConnect" />
      </template>
      <div class="row between">
        <button type="button" class="adv-toggle" @click="showAdvanced = !showAdvanced">
          {{ showAdvanced ? '返回简易模式' : '高级：输入完整地址' }}
        </button>
        <MdButton :disabled="busy" @click="doConnect">{{ busy ? '连接中…' : '连接' }}</MdButton>
      </div>
      <p class="discover-hint">浏览器无法进行局域网自动扫描，请在猫猫屏幕或路由器上查看设备 IP。手机 App 支持局域网自动发现。</p>

      <p v-if="link.lastError" class="err">{{ link.lastError }}</p>
    </MdCard>
  </div>
</template>

<style scoped>
.connect-wrap {
  max-width: 420px;
  margin: 10vh auto 0;
  padding: 0 20px;
  text-align: center;
}
.brand {
  font-size: 44px;
  color: var(--md-primary);
  letter-spacing: 2px;
}
.sub {
  color: var(--md-on-surface-variant);
  margin: 6px 0 26px;
}
.card {
  text-align: left;
}
.lbl {
  display: block;
  font-size: 12.5px;
  color: var(--md-on-surface-variant);
  margin-bottom: 8px;
  letter-spacing: 1px;
}
.addr-row {
  display: flex;
  gap: 10px;
}
.field.grow { flex: 1; min-width: 0; }
.field.port { width: 92px; flex: none; }
.preview {
  margin-top: 8px;
  font-size: 12px;
  color: var(--md-on-surface-variant);
  font-family: monospace;
}
.row {
  margin-top: 14px;
  display: flex;
  justify-content: flex-end;
}
.row.between {
  justify-content: space-between;
  align-items: center;
}
.adv-toggle {
  background: transparent;
  border: none;
  color: var(--md-primary);
  font-size: 12.5px;
  cursor: pointer;
  padding: 4px 0;
}
.discover-hint {
  margin-top: 14px;
  font-size: 12px;
  color: var(--md-on-surface-variant);
  line-height: 1.6;
}
.err {
  color: var(--md-error);
  font-size: 13px;
  margin: 14px 0 0;
}
</style>
