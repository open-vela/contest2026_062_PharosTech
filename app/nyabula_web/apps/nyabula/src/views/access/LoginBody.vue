<script setup lang="ts">
/* Login content shared by the phone and desktop variants; the variants only
 * differ in the frame around it. */
import { nextTick, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, MdSwitch, MdTextField, UiIcon } from '@nyabula/ui';
import type { LoginPage } from './access.logic';

const props = defineProps<{ page: LoginPage; thumb?: boolean }>();
const field = ref<{ focus: () => void } | null>(null);
/* A wrong password keeps the focus in the field. The form is the only submit
 * trigger (Enter and the button both go through it). */
watch(() => props.page.refocus.value, () => void nextTick(() => field.value?.focus()));
</script>

<template>
  <div class="stack body">
    <!-- The only place that asks for the QR code: nothing to log in to yet. -->
    <MdCard v-if="page.noPassword.value">
      <EmptyState
        icon="qr_code"
        title="这台设备还没有设置访问密码"
        :hint="page.onComputer.value
          ? '需要设备主人先用手机扫描设备眼睛屏幕上的二维码，设置一个访问密码。电脑扫不了码，请先在手机上完成这一步，再回到这里用密码登录。'
          : '请用手机相机扫描设备眼睛屏幕上的二维码，从扫出来的链接进入并设置一个访问密码。'"
        :action-text="page.checking.value ? '正在检查…' : '已经设置好了，重新检查'"
        @action="page.check(true)"
      />
    </MdCard>

    <MdCard v-else>
      <form class="stack form" :class="{ thumb }" @submit.prevent="page.submit()">
        <MdTextField
          ref="field"
          v-model="page.password.value"
          label="访问密码"
          :type="page.show.value ? 'text' : 'password'"
          icon="key"
          autocomplete="current-password"
          :disabled="page.locked.value || page.busy.value"
        />
        <MdSwitch v-model="page.show.value" class="show" label="显示密码" />

        <p v-if="page.locked.value" class="notice" role="status">
          <UiIcon name="lock" :size="18" /> <span>输错的次数太多了，请等 {{ page.lockText.value }} 后再试。</span>
        </p>
        <p v-else-if="page.error.value" class="notice err" role="alert">
          <UiIcon name="error" :size="18" /> <span>{{ page.error.value }}</span>
        </p>

        <MdButton class="submit" type="submit" :disabled="!page.canSubmit.value">{{ page.busy.value ? '正在登录…' : '登录' }}</MdButton>
      </form>
    </MdCard>

    <p v-if="!page.noPassword.value" class="muted foot">忘记密码？用手机扫描设备眼睛屏幕上的二维码，可以重新设置。</p>
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
.foot { font-size: 12.5px; line-height: 1.6; margin: 0; text-align: center; }
</style>
