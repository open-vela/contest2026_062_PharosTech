<script setup lang="ts">
/* Nebula connect: centred hero with a free-floating pair of eyes that
 * follows the pointer and reacts to the flow (curious / sleepy with no
 * devices / thinking while connecting / sad on error), then two glass
 * cards: recent devices and "add device" (LAN | Cloud segment).
 * Parity with views/connect (same useConnectPage): known devices (open,
 * forget, current), LAN host+port or full ws URL, access token, validation
 * + errors, connecting state, re-auth prefill, cloud login CTA / claimed
 * device list with online state, UDP discovery hint. */
import { computed, inject, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { MdSwitch, UiIcon } from '@nyabula/ui';
import type { useFormFactor } from '../../../composables/useFormFactor';
import { useConnectPage } from '../../../views/connect/connect.logic';
import NebulaEyeCanvas from '../parts/NebulaEyeCanvas.vue';

const page = useConnectPage();
const { session, account, known, cloudDevices, host, port, advanced, fullUrl, accessToken, connecting, error } = page;
const ff = inject<ReturnType<typeof useFormFactor>>('formFactor')!;
const phone = computed(() => ff.formFactor.value === 'phone');

onMounted(() => {
  if (account.loggedIn) void account.refreshDevices().catch(() => undefined);
});

type Via = 'lan' | 'cloud';
const via = ref<Via>('lan');
const mood = computed(() => (connecting.value ? 'processing' : error.value ? 'sad' : known.value.length ? 'curious' : 'sleepy'));

function ago(t: number): string {
  const m = Math.round((Date.now() - t) / 60000);
  if (m < 1) return '刚刚';
  if (m < 60) return `${m} 分钟前`;
  const h = Math.round(m / 60);
  if (h < 24) return `${h} 小时前`;
  return `${Math.round(h / 24)} 天前`;
}
</script>

<template>
  <div class="nb-connect" :class="{ phone }">
    <section class="hero">
      <div class="eyes" data-eyes-anchor :data-eyes-mode="mood">
        <div class="halo" />
        <NebulaEyeCanvas :mode="mood" toy-mode />
      </div>
      <div class="copy nb-in" style="--d: 120ms">
        <p class="nb-kicker">Nyabula · 星喵</p>
        <h1 class="nb-title nb-grad headline">{{ page.reauth ? '重新认证你的 Nyabula' : '唤醒你的 Nyabula' }}</h1>
        <p class="sub">局域网直连，或经 Nyabula Cloud 远程接入。同一工作区，两种方式。</p>
      </div>
    </section>

    <section class="cols">
      <article class="card nb-glass nb-in" style="--d: 200ms">
        <div class="card-head">
          <p class="nb-kicker">最近的设备</p>
          <span class="count">{{ known.length }}</span>
        </div>
        <div v-if="known.length" class="known">
          <div
            v-for="(d, i) in known"
            :key="d.key"
            class="dev"
            :class="{ current: d.key === session.deviceKey }"
            :style="{ animationDelay: 260 + i * 60 + 'ms' }"
            role="button"
            tabindex="0"
            @click="page.open(d.key)"
            @keydown.enter="page.open(d.key)"
          >
            <span class="dev-ico"><UiIcon :name="d.transport === 'cloud' ? 'cloud' : 'wifi'" :size="20" /></span>
            <span class="dev-body">
              <span class="dev-name">{{ d.label }}</span>
              <span class="dev-sub mono">{{ d.address }}</span>
              <span class="dev-meta">{{ d.transport === 'cloud' ? '云中继' : '局域网' }} · {{ ago(d.lastSeen) }}<template v-if="d.coreVersion"> · Core {{ d.coreVersion }}</template></span>
            </span>
            <button class="forget" title="忘记" @click.stop="page.forget(d.key)"><UiIcon name="delete" :size="17" /></button>
            <UiIcon name="arrow_forward" :size="18" class="go" />
          </div>
        </div>
        <div v-else class="empty">
          <span class="empty-orb"><UiIcon name="devices" :size="22" /></span>
          <b>还没有连接过设备</b>
          <span>在「添加设备」输入局域网 IP，或登录账号从云端选择。</span>
        </div>
      </article>

      <article class="card nb-glass nb-in" style="--d: 260ms">
        <div class="card-head">
          <p class="nb-kicker">添加设备</p>
        </div>
        <div class="nb-seg" :style="{ '--n': 2, '--i': via === 'lan' ? 0 : 1 }">
          <span class="seg-light" />
          <button :class="{ on: via === 'lan' }" @click="via = 'lan'"><UiIcon name="wifi" :size="16" /> 局域网直连</button>
          <button :class="{ on: via === 'cloud' }" @click="via = 'cloud'"><UiIcon name="cloud" :size="16" /> Nyabula Cloud</button>
        </div>

        <Transition name="nb-page" mode="out-in">
          <form v-if="via === 'lan'" key="lan" class="form" @submit.prevent="page.connectLan()">
            <template v-if="!advanced">
              <label class="field host">
                <span>设备 IP / 主机名</span>
                <span class="with-ico"><UiIcon name="wifi" :size="17" /><input v-model="host" class="nb-input" placeholder="192.168.1.88" inputmode="url" autocomplete="off" /></span>
              </label>
              <label class="field port">
                <span>端口</span>
                <input v-model="port" class="nb-input" placeholder="7788" inputmode="numeric" />
              </label>
            </template>
            <label v-else class="field">
              <span>完整 WebSocket 地址</span>
              <span class="with-ico"><UiIcon name="link" :size="17" /><input v-model="fullUrl" class="nb-input" placeholder="ws://192.168.1.88:7788/nyalink" autocomplete="off" /></span>
            </label>
            <label class="field">
              <span>原生 Core 访问令牌（可选）</span>
              <input v-model="accessToken" class="nb-input" type="password" autocomplete="off" />
              <small>使用设备端预配置的令牌；仅留在本次会话内存中</small>
            </label>
            <Transition name="nb-pop"><p v-if="error" class="err"><UiIcon name="error" :size="16" /> {{ error }}</p></Transition>
            <div class="form-foot">
              <MdSwitch v-model="advanced" label="高级：手动填写地址" />
              <button type="submit" class="nb-btn primary connect" :disabled="connecting">
                <span v-if="connecting" class="spin" />{{ connecting ? '连接中…' : '连接' }}
              </button>
            </div>
            <p class="hint"><UiIcon name="info" :size="14" /> 浏览器无法收听 UDP 发现广播；设备 IP 可在设备屏幕「网络」场景或路由器中查看。</p>
          </form>

          <div v-else key="cloud" class="cloud">
            <template v-if="account.loggedIn">
              <div v-if="cloudDevices.length" class="known">
                <button v-for="d in cloudDevices" :key="d.deviceId" class="dev" :disabled="!d.online" @click="page.openCloud(d.deviceId)">
                  <span class="dev-ico"><UiIcon name="cloud" :size="20" /></span>
                  <span class="dev-body">
                    <span class="dev-name">{{ d.name }}</span>
                    <span class="dev-sub mono">{{ d.deviceId }}</span>
                  </span>
                  <span class="nb-tag" :class="d.online ? 'ok' : ''"><span class="nb-dot" />{{ d.online ? '在线' : '离线' }}</span>
                </button>
              </div>
              <p v-else class="cloud-empty">账号下还没有认领的设备。<RouterLink to="/account">去认领</RouterLink></p>
            </template>
            <div v-else class="cloud-login">
              <span class="empty-orb"><UiIcon name="cloud" :size="22" /></span>
              <b>登录 Nyabula Cloud</b>
              <span>登录后可在外网远程连接已认领的设备。</span>
              <RouterLink to="/account" class="nb-btn primary">登录</RouterLink>
            </div>
          </div>
        </Transition>
      </article>
    </section>
  </div>
</template>

<style scoped>
.nb-connect { display: flex; flex-direction: column; gap: 26px; padding: 0 clamp(16px, 3vw, 36px) 24px; max-width: 1180px; margin: 0 auto; width: 100%; }
.hero { display: grid; grid-template-rows: minmax(200px, 30vh) auto; justify-items: center; text-align: center; }
.eyes { position: relative; width: min(620px, 100%); height: 100%; }
.eyes :deep(.nb-eye-canvas) { position: absolute; inset: 0; }
.halo { position: absolute; inset: 14% 20%; border-radius: 50%; background: radial-gradient(closest-side, rgba(var(--nb-acc-rgb), 0.18), transparent); filter: blur(30px); animation: nb-breathe 6s ease-in-out infinite; pointer-events: none; }
@keyframes nb-breathe { 50% { transform: scale(1.08); opacity: 0.7; } }
.headline { font-size: clamp(32px, 4.4vw, 56px); margin: 8px 0 8px; line-height: 1.1; }
.sub { margin: 0; color: var(--nb-sub); font-size: 14px; }

.cols { display: grid; grid-template-columns: 1fr 1.1fr; gap: 18px; align-items: start; }
.card { padding: 20px 22px; display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.card-head { display: flex; align-items: center; justify-content: space-between; }
.count { font: 600 12px var(--font-body); color: var(--nb-sub); padding: 2px 9px; border-radius: 999px; background: var(--nb-soft-2); }

.known { display: grid; gap: 8px; }
.dev {
  display: flex;
  align-items: center;
  gap: 12px;
  width: 100%;
  padding: 12px 12px 12px 12px;
  border-radius: 18px;
  border: 1px solid var(--nb-line);
  background: var(--nb-soft);
  color: var(--nb-fg);
  cursor: pointer;
  text-align: left;
  font: inherit;
  animation: nb-rise 0.6s var(--nb-ease) both;
  transition: background 0.25s, border-color 0.25s, transform 0.35s var(--nb-spring);
}
.dev:hover:not(:disabled) { background: var(--nb-soft-2); border-color: var(--nb-line-strong); transform: translateX(3px); }
.dev:disabled { opacity: 0.5; cursor: default; }
.dev.current { border-color: rgba(var(--nb-acc-rgb), 0.55); box-shadow: 0 0 26px -10px rgba(var(--nb-acc-rgb), 0.8); }
.dev-ico { width: 42px; height: 42px; border-radius: 14px; display: grid; place-items: center; background: rgba(var(--nb-acc-rgb), 0.13); color: var(--nb-acc); flex: none; }
.dev-body { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 1px; }
.dev-name { font: 600 14.5px var(--font-body); }
.dev-sub { font-size: 12px; color: var(--nb-sub); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.dev-meta { font-size: 11.5px; color: var(--nb-dim); }
.forget { width: 34px; height: 34px; border: none; border-radius: 50%; background: transparent; color: var(--nb-sub); display: grid; place-items: center; cursor: pointer; opacity: 0; transition: opacity 0.2s, background 0.2s, color 0.2s; }
.dev:hover .forget, .dev:focus-within .forget { opacity: 1; }
.forget:hover { background: color-mix(in srgb, var(--md-error) 14%, transparent); color: var(--md-error); }
.go { color: var(--nb-dim); transition: transform 0.35s var(--nb-spring), color 0.2s; }
.dev:hover .go { color: var(--nb-acc); transform: translateX(3px); }
.phone .forget { opacity: 1; }

.empty, .cloud-login { display: flex; flex-direction: column; align-items: center; text-align: center; gap: 6px; padding: 22px 10px; color: var(--nb-sub); font-size: 13px; }
.empty b, .cloud-login b { color: var(--nb-fg); font: 600 15px var(--font-body); margin-top: 6px; }
.cloud-login .nb-btn { margin-top: 10px; text-decoration: none; padding: 10px 26px; }
.empty-orb { width: 52px; height: 52px; border-radius: 50%; display: grid; place-items: center; color: var(--nb-acc); background: radial-gradient(circle at 35% 30%, rgba(var(--nb-acc-rgb), 0.35), rgba(var(--nb-acc-rgb), 0.06) 70%); box-shadow: 0 0 0 1px rgba(var(--nb-acc-rgb), 0.2), 0 0 40px -10px rgba(var(--nb-acc-rgb), 0.6); }

.nb-seg > button { display: inline-flex; align-items: center; justify-content: center; gap: 6px; }
.form { display: grid; grid-template-columns: 1fr 120px; gap: 14px 12px; }
.field { display: flex; flex-direction: column; gap: 7px; grid-column: 1 / -1; min-width: 0; }
.field > span:first-child { font: 600 12.5px var(--font-body); color: var(--nb-sub); }
.field small { font-size: 11.5px; color: var(--nb-dim); }
.form .field.host { grid-column: 1; }
.form .field.port { grid-column: 2; }
.with-ico { position: relative; display: block; }
.with-ico :deep(.ui-icon) { position: absolute; left: 13px; top: 50%; transform: translateY(-50%); color: var(--nb-dim); pointer-events: none; }
.with-ico .nb-input { padding-left: 40px; }
.err { grid-column: 1 / -1; margin: 0; display: flex; align-items: center; gap: 6px; color: var(--md-error); font-size: 13px; }
.form-foot { grid-column: 1 / -1; display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.connect { min-width: 112px; padding: 11px 22px; }
.spin { width: 14px; height: 14px; border-radius: 50%; border: 2px solid currentColor; border-top-color: transparent; animation: nb-spin 0.8s linear infinite; }
@keyframes nb-spin { to { transform: rotate(360deg); } }
.hint { grid-column: 1 / -1; margin: 0; display: flex; gap: 6px; align-items: flex-start; font-size: 12px; line-height: 1.5; color: var(--nb-dim); }
.cloud-empty { margin: 8px 0; color: var(--nb-sub); font-size: 13px; }
.cloud-empty a { color: var(--nb-acc); }

.phone { gap: 18px; padding: 0 14px 16px; }
.phone .hero { grid-template-rows: 22vh auto; }
.phone .headline { font-size: 30px; }
.phone .cols { grid-template-columns: 1fr; }
.phone .form { grid-template-columns: 1fr 96px; }
@media (max-width: 960px) {
  .cols { grid-template-columns: 1fr; }
}
</style>
