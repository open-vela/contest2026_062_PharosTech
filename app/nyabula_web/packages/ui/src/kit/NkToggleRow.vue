<script setup lang="ts">
/* NkRow + MdSwitch bound via v-model. Tapping anywhere on the row toggles. */
import MdSwitch from '../components/MdSwitch.vue';
import NkRow from './NkRow.vue';

export interface NkToggleRowProps {
  modelValue: boolean;
  icon?: string;
  title: string;
  sub?: string;
  disabled?: boolean;
}
const props = withDefaults(defineProps<NkToggleRowProps>(), { disabled: false });
const emit = defineEmits<{ (e: 'update:modelValue', v: boolean): void }>();
function set(v: boolean): void {
  if (!props.disabled) emit('update:modelValue', v);
}
</script>

<template>
  <NkRow :icon="icon" :title="title" :sub="sub" :disabled="disabled" tappable @tap="set(!modelValue)">
    <span class="nk-toggle-wrap" @click.stop>
      <MdSwitch :model-value="modelValue" :disabled="disabled" @update:model-value="set" />
    </span>
  </NkRow>
</template>

<style scoped>
.nk-toggle-wrap { display: inline-flex; align-items: center; min-height: 44px; }
</style>
