<script setup lang="ts">
/* Access password (device build): sys.password.set {password,current}. The
 * device answers with a new session token that replaces the stored one; every
 * other logged-in browser has to log in again. Never log a password. */
import { computed, ref } from 'vue';
import { MdButton, MdCard, MdSwitch, MdTextField, UiIcon, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { useDeviceAccessStore } from '../../../stores/deviceAccess';
import { PASSWORD_MAX, PASSWORD_MIN, validatePassword } from '../../../lib/deviceAccess';
import { passwordChangeError } from '../../access/access.logic';

const session = useSessionStore();
const access = useDeviceAccessStore();
const toast = useToastStore();

const current = ref('');
const password = ref('');
const confirm = ref('');
const show = ref(false);
const busy = ref(false);
const error = ref<string | null>(null);

/* A socket that said hello with the pair token (QR code scanned in this tab)
 * needs no current password; one that logged in does. The device enforces
 * this per socket, so its own `auth` answer from sys.hello wins; the locally
 * chosen credential is only the fallback when the device did not say. */
const needsCurrent = computed(() => (session.helloAuth ?? session.credential) !== 'pair');
const canSubmit = computed(() => session.connected && session.isOwner && !busy.value && !!password.value && !!confirm.value && (!needsCurrent.value || !!current.value));

async function save(): Promise<void> {
  if (!canSubmit.value) return;
  error.value = validatePassword(password.value, confirm.value);
  if (error.value) return;
  busy.value = true;
  try {
    await access.setPassword(password.value, needsCurrent.value ? current.value : undefined);
    current.value = '';
    password.value = '';
    confirm.value = '';
    toast.ok('访问密码已修改');
  } catch (e) {
    error.value = passwordChangeError(e, '当前密码不对。');
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="stack">
    <MdCard title="访问密码">
      <div class="head">
        <span class="orb"><UiIcon name="key" :size="24" /></span>
        <div class="head-body">
          <div class="head-title">{{ access.passwordSet === false ? '还没有设置密码' : '已设置访问密码' }}</div>
          <div class="muted sub">在手机或电脑的浏览器里打开设备地址，输入这个密码就能进入，不用扫码。</div>
        </div>
      </div>
    </MdCard>

    <MdCard title="修改密码">
      <form class="stack form" @submit.prevent="save()">
        <MdTextField v-if="needsCurrent" v-model="current" label="当前密码" :type="show ? 'text' : 'password'" icon="lock" autocomplete="current-password" :disabled="busy" />
        <p v-else class="muted note">你是扫描设备上的二维码进来的，不需要输入当前密码。</p>
        <MdTextField v-model="password" label="新密码" :type="show ? 'text' : 'password'" :placeholder="`${PASSWORD_MIN}–${PASSWORD_MAX} 位`" icon="key" autocomplete="new-password" :disabled="busy" />
        <MdTextField v-model="confirm" label="再输入一次新密码" :type="show ? 'text' : 'password'" icon="key" autocomplete="new-password" :disabled="busy" />
        <MdSwitch v-model="show" label="显示密码" />
        <p v-if="error" class="form-error" role="alert">{{ error }}</p>
        <div class="row" style="justify-content: flex-end; gap: 10px">
          <span v-if="session.connected && !session.isOwner" class="muted note">仅主人 (owner) 可修改</span>
          <MdButton type="submit" :disabled="!canSubmit">{{ busy ? '保存中…' : '保存新密码' }}</MdButton>
        </div>
      </form>
      <p class="muted note foot">修改后，其他已经登录的手机和电脑需要用新密码重新登录。忘记当前密码时，用手机扫描设备眼睛屏幕上的二维码可以重新设置。</p>
    </MdCard>
  </div>
</template>

<style scoped>
.head { display: flex; align-items: center; gap: 14px; }
.orb { width: 48px; height: 48px; border-radius: 50%; display: grid; place-items: center; flex: none; background: rgba(var(--md-primary-rgb), 0.14); color: var(--md-primary); }
.head-body { flex: 1; min-width: 0; }
.head-title { font: 600 16px var(--font-title); color: var(--md-on-surface); }
.sub { font-size: 12.5px; margin-top: 2px; line-height: 1.5; }
.form { gap: 12px; }
.note { font-size: 12.5px; margin: 0; line-height: 1.6; }
.foot { margin-top: 12px; }
.form-error { margin: 0; font-size: 13px; color: var(--md-error); }
</style>
