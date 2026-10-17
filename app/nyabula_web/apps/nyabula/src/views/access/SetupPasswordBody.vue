<script setup lang="ts">
/* Set / reset the access password after scanning the QR code; shared by the
 * phone and desktop variants. The form is the only submit trigger. */
import { MdButton, MdCard, MdSwitch, MdTextField, UiIcon } from '@nyabula/ui';
import { PASSWORD_MAX, PASSWORD_MIN } from '../../lib/deviceAccess';
import type { SetupPasswordPage } from './access.logic';

defineProps<{ page: SetupPasswordPage; thumb?: boolean }>();
</script>

<template>
  <div class="stack body">
    <MdCard>
      <form class="stack form" :class="{ thumb }" @submit.prevent="page.submit()">
        <MdTextField
          v-model="page.password.value"
          label="新密码"
          :type="page.show.value ? 'text' : 'password'"
          :placeholder="`${PASSWORD_MIN}–${PASSWORD_MAX} 位`"
          icon="key"
          autocomplete="new-password"
          :disabled="page.busy.value"
        />
        <MdTextField
          v-model="page.confirm.value"
          label="再输入一次"
          :type="page.show.value ? 'text' : 'password'"
          icon="key"
          autocomplete="new-password"
          :disabled="page.busy.value"
        />
        <MdSwitch v-model="page.show.value" class="show" label="显示密码" />

        <p v-if="page.error.value" class="notice err" role="alert"><UiIcon name="error" :size="18" /> <span>{{ page.error.value }}</span></p>
        <p v-else-if="!page.session.connected" class="notice" role="status"><UiIcon name="sync" :size="18" /> <span>与设备的连接中断了，正在重连…</span></p>

        <MdButton class="submit" type="submit" :disabled="!page.canSubmit.value">{{ page.busy.value ? '正在保存…' : page.reset.value ? '保存新密码' : '设置密码并继续' }}</MdButton>
        <MdButton v-if="page.reset.value" class="submit secondary" type="button" variant="text" :disabled="page.busy.value" @click="page.skip()">不修改，直接进入</MdButton>
      </form>
    </MdCard>

    <p class="muted foot">
      <template v-if="page.reset.value">保存新密码后，其他已经登录的手机和电脑需要用新密码重新登录。选择“不修改，直接进入”只对当前这个页面有效，关掉后需要密码或重新扫码。</template>
      <template v-else>以后在手机或电脑的浏览器里打开设备地址，输入这个密码就能进入，不用再扫码。</template>
    </p>
  </div>
</template>

<style scoped>
.body { gap: 14px; }
.form { gap: 12px; }
/* Phone (`thumb`): the submit button comes right after the field(s), so it
 * stays above the keyboard; the show-password switch moves below it. */
.form.thumb .show { order: 1; }
.notice { display: flex; align-items: flex-start; gap: 8px; margin: 0; padding: 10px 14px; border-radius: var(--radius-m); font-size: 13.5px; line-height: 1.5; color: var(--md-on-surface); background: color-mix(in srgb, var(--md-warning) 16%, var(--md-surface-container)); }
.notice.err { background: color-mix(in srgb, var(--md-error) 16%, var(--md-surface-container)); }
.notice .ui-icon { flex: none; margin-top: 1px; }
.submit { width: 100%; padding: 13px 22px; }
.foot { font-size: 12.5px; line-height: 1.6; margin: 0; }
</style>
