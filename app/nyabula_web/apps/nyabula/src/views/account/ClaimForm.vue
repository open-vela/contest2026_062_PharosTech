<script setup lang="ts">
/* deviceId + claimCode form. Emits `done` after a successful claim. */
import { MdButton, MdTextField } from '@nyabula/ui';
import type { useAccountPage } from './account.logic';

const props = defineProps<{ page: ReturnType<typeof useAccountPage> }>();
const emit = defineEmits<{ (e: 'done'): void }>();
const p = props.page;

async function submit() {
  if (await p.submitClaim()) emit('done');
}
</script>

<template>
  <form class="stack" @submit.prevent="submit">
    <MdTextField v-model="p.claimId.value" label="设备 ID" icon="smartphone" placeholder="nya-xxxxxxxx" hint="设备「关于」页或首次开机时眼睛上显示" autocomplete="off" />
    <MdTextField v-model="p.claimCode.value" label="认领码" icon="key" placeholder="6 位认领码" inputmode="numeric" autocomplete="off" @enter="submit" />
    <MdButton :disabled="!p.claimValid.value || p.claimBusy.value">{{ p.claimBusy.value ? '认领中…' : '认领设备' }}</MdButton>
  </form>
</template>
