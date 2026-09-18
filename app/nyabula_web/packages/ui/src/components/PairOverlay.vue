<script setup lang="ts">
/* Centered pairing overlay shown over the eye display area when the link
 * reports pairingRequired. Renders a 6-cell code input; submits via the
 * confirm button or Enter. On error the cells shake and a red message is
 * shown; the code stays editable so the user can retry in place. */
import { computed, nextTick, ref, watch } from 'vue';
import MdButton from './MdButton.vue';

const props = defineProps<{
  /** True while a pair request is in flight (disables input + button). */
  busy?: boolean;
  /** Error message to show under the cells (null/'' = no error). */
  error?: string | null;
}>();

const emit = defineEmits<{ (e: 'submit', code: string): void }>();

const code = ref('');
const inputRef = ref<HTMLInputElement | null>(null);
const shaking = ref(false);

const digits = computed(() =>
  Array.from({ length: 6 }, (_, i) => code.value[i] ?? ''),
);
const canSubmit = computed(() => code.value.length === 6 && !props.busy);

function onInput(e: Event) {
  const el = e.target as HTMLInputElement;
  code.value = el.value.replace(/\D/g, '').slice(0, 6);
  el.value = code.value;
}

function submit() {
  if (canSubmit.value) emit('submit', code.value);
}

function focusInput() {
  inputRef.value?.focus();
}

watch(
  () => props.error,
  (err) => {
    if (!err) return;
    // Retrigger the CSS shake even for repeated identical errors.
    shaking.value = false;
    void nextTick(() => {
      shaking.value = true;
    });
    focusInput();
  },
);
</script>

<template>
  <div class="pair-overlay" @click="focusInput">
    <div class="pair-box">
      <h3 class="pair-title">请输入配对码</h3>
      <div class="cells" :class="{ shake: shaking, err: !!error }" @animationend="shaking = false">
        <span
          v-for="(d, i) in digits"
          :key="i"
          class="cell"
          :class="{ filled: d !== '', caret: i === code.length && !busy }"
        >{{ d }}</span>
        <input
          ref="inputRef"
          class="ghost-input"
          type="text"
          inputmode="numeric"
          autocomplete="one-time-code"
          maxlength="6"
          :disabled="busy"
          :value="code"
          @input="onInput"
          @keyup.enter="submit"
        />
      </div>
      <p v-if="error" class="pair-err">{{ error }}</p>
      <MdButton variant="tonal" :disabled="!canSubmit" @click="submit">
        {{ busy ? '配对中…' : '确认' }}
      </MdButton>
      <p class="pair-hint">配对码显示在设备屏幕上</p>
    </div>
  </div>
</template>

<style scoped>
.pair-overlay {
  position: absolute;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 5;
}
.pair-box {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 14px;
  padding: 26px 30px;
  border-radius: var(--radius-l);
  background: var(--md-surface-container-high);
  box-shadow: var(--md-elev-2, 0 4px 16px rgba(0, 0, 0, 0.35));
}
.pair-title {
  font: 600 17px var(--font-title, var(--font-body));
  color: var(--md-on-surface);
  margin: 0;
}
.cells {
  position: relative;
  display: flex;
  gap: 8px;
  cursor: text;
}
.cell {
  width: 40px;
  height: 52px;
  display: flex;
  align-items: center;
  justify-content: center;
  font: 700 26px var(--font-body);
  color: var(--md-on-surface);
  background: var(--md-surface-container);
  border: 1.5px solid var(--md-outline-variant);
  border-radius: var(--radius-m);
}
.cell.filled {
  border-color: var(--md-primary);
}
.cell.caret {
  border-color: var(--md-primary);
  box-shadow: 0 0 0 1px var(--md-primary) inset;
}
.cells.err .cell {
  border-color: var(--md-error);
}
.ghost-input {
  position: absolute;
  inset: 0;
  opacity: 0;
  border: none;
  background: transparent;
  font-size: 26px;
  cursor: text;
}
.cells.shake {
  animation: pair-shake 0.4s ease;
}
@keyframes pair-shake {
  0%, 100% { transform: translateX(0); }
  20% { transform: translateX(-7px); }
  40% { transform: translateX(6px); }
  60% { transform: translateX(-4px); }
  80% { transform: translateX(3px); }
}
.pair-err {
  color: var(--md-error);
  font-size: 13px;
  margin: 0;
}
.pair-hint {
  color: var(--md-on-surface-variant);
  font-size: 12px;
  margin: 0;
}
</style>
