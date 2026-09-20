<script setup lang="ts">
import { RouterLink } from 'vue-router';
import { MdButton, MdCard, MdSwitch, MdTextField, UiIcon } from '@nyabula/ui';
import type { useConnectPage } from './connect.logic';

const props = defineProps<{ page: ReturnType<typeof useConnectPage> }>();
const { host, port, advanced, fullUrl, connecting, error, connectLan, account, cloudDevices, openCloud } = props.page;
</script>

<template>
  <div class="stack" style="gap: 14px">
    <MdCard title="局域网直连">
      <div class="stack" style="gap: 12px">
        <template v-if="!advanced">
          <MdTextField v-model="host" label="设备 IP / 主机名" placeholder="192.168.1.88" icon="wifi" inputmode="url" :error="error" @enter="connectLan" />
          <MdTextField v-model="port" label="端口" placeholder="7788" inputmode="numeric" @enter="connectLan" />
        </template>
        <MdTextField v-else v-model="fullUrl" label="完整 WebSocket 地址" placeholder="ws://192.168.1.88:7788/nyalink" icon="link" :error="error" @enter="connectLan" />
        <MdTextField v-model="page.accessToken.value" type="password" label="原生 Core 访问令牌（可选）" hint="使用设备端预配置的令牌；仅留在本次会话内存中" @enter="connectLan" />
        <div class="row between">
          <MdSwitch v-model="advanced" label="高级：手动填写地址" />
          <MdButton :disabled="connecting" @click="connectLan">{{ connecting ? '连接中…' : '连接' }}</MdButton>
        </div>
        <p class="muted hint"><UiIcon name="info" :size="14" /> 浏览器无法收听 UDP 发现广播；设备 IP 可在设备屏幕「网络」场景或路由器中查看。</p>
      </div>
    </MdCard>

    <MdCard title="通过 Nyabula Cloud">
      <template v-if="account.loggedIn">
        <div v-if="cloudDevices.length" class="stack">
          <button v-for="d in cloudDevices" :key="d.deviceId" class="list-tile" :disabled="!d.online" @click="openCloud(d.deviceId)">
            <span class="tile-icon"><UiIcon name="cloud" :size="20" /></span>
            <span class="tile-body"><span class="tile-title">{{ d.name }}</span><span class="tile-sub mono">{{ d.deviceId }}</span></span>
            <span class="tile-trail"><span class="tag" :class="d.online ? 'ok' : ''">{{ d.online ? '在线' : '离线' }}</span></span>
          </button>
        </div>
        <p v-else class="muted" style="font-size: 13px; margin: 0">账号下还没有认领的设备。<RouterLink to="/account">去认领</RouterLink></p>
      </template>
      <div v-else class="row between">
        <span class="muted" style="font-size: 13.5px">登录后可远程连接已认领的设备</span>
        <RouterLink to="/account"><MdButton variant="tonal">登录</MdButton></RouterLink>
      </div>
    </MdCard>
  </div>
</template>

<style scoped>
.hint { font-size: 12px; margin: 0; display: flex; gap: 6px; align-items: flex-start; line-height: 1.5; }
</style>
