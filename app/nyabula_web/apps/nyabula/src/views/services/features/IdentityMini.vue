<script setup lang="ts">
import { MdButton,UiIcon } from '@nyabula/ui';
import { useIdentityStore } from '../../../stores/identity';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import type { FeatureMiniProps } from './contract';
defineProps<FeatureMiniProps>();defineEmits<{(e:'open'):void}>();
const identity=useIdentityStore();useVisiblePoll(()=>void identity.refresh(),1500);
</script>
<template><div class="identity-mini"><div><strong>{{ type==='presence'?(identity.recent.length?identity.recent.map(m=>m.name).join('、'):'镜头前未确认家人'):identity.label }}</strong><small>{{ identity.error||identity.readError||(type==='presence'?'近 3 秒识别结果，与身份档案同步':'本地登记与识别') }}</small></div><MdButton variant="tonal" @click="$emit('open')">{{ type==='presence'?'查看':'管理' }}<UiIcon name="chevron_right" :size="16"/></MdButton></div></template>
<style scoped>.identity-mini{display:flex;align-items:center;gap:12px;min-height:54px}.identity-mini>div{flex:1;min-width:0;display:grid;gap:5px}.identity-mini strong{font-size:14px;overflow-wrap:anywhere}.identity-mini small{font-size:12px;color:var(--md-on-surface-variant);line-height:1.5}</style>
