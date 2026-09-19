<script setup lang="ts">
/* Pairing mini: explanation only. The device shows the pairing QR (scene `qr`)
 * by itself and the panel never handles the access token, so the card has no
 * action; it reflects whether the QR is up and who is connected. */
import { computed } from 'vue';
import { UiIcon } from '@nyabula/ui';
import { useEyeStore } from '../../../stores/eye';
import { useSessionStore } from '../../../stores/session';
import type { FeatureMiniProps } from './contract';

defineProps<FeatureMiniProps>();
const eye = useEyeStore();
const session = useSessionStore();
const showing = computed(() => eye.activeScene === 'qr');
const ROLE_LABEL: Record<string, string> = { owner: '主人', family: '家人', guest: '访客' };
const role = computed(() => (session.role ? ROLE_LABEL[session.role] ?? session.role : '未配对'));
</script>

<template>
  <div class="pam">
    <span class="pam-note">
      <UiIcon :name="showing ? 'qr_code' : 'lock'" :size="16" :class="{ on: showing }" />
      {{ showing ? '二维码正在猫眼上显示' : '需要时设备会自己显示二维码' }}
    </span>
    <span class="pam-role">我的身份：{{ role }}</span>
  </div>
</template>

<style scoped>
.pam-role { flex: none; font-size: 12.5px; color: var(--md-on-surface-variant); }
.pam { display: flex; align-items: center; gap: 10px; min-height: 44px; }
.pam-note { flex: 1; min-width: 0; display: inline-flex; align-items: center; gap: 6px; font-size: 12.5px; color: var(--md-on-surface-variant); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.pam-note .on { color: var(--md-primary); }
.pam-btn { flex: none; min-height: 40px; padding: 0 16px; display: inline-flex; align-items: center; gap: 4px; }
</style>
