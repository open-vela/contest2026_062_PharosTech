<script setup lang="ts">
/* Iris colour: one colour or one per eye, the sets that come with the panel,
 * the owner's own sets and what was worn lately.  The device keeps the colour
 * itself; sets and history are this browser's, per device. */
import { computed, ref, watch } from 'vue';
import { MdCard, MdButton, UiIcon, useDialogStore } from '@nyabula/ui';
import { useEyeStore } from '../../stores/eye';
import { useSessionStore } from '../../stores/session';
import { BUILTIN_IRIS_SETS, DEFAULT_IRIS, loadIrisBook, normalizeHex, pairKey, rememberPair, saveIrisBook, type IrisPair } from './iris';

const eye = useEyeStore();
const session = useSessionStore();
const dialog = useDialogStore();

const left = ref(DEFAULT_IRIS);
const right = ref(DEFAULT_IRIS);
/** Two colours: the wells stop following each other. */
const odd = ref(false);
const book = ref(loadIrisBook(session.deviceKey));
watch(() => session.deviceKey, key => { book.value = loadIrisBook(key); });

/* The device is the authority on what is worn; a well being dragged is not
 * pulled out from under the pointer. */
const dragging = ref(false);
watch(() => eye.irisColors, colors => {
  if (!colors || dragging.value) return;
  left.value = colors.left;
  right.value = colors.right;
  if (colors.left !== colors.right) odd.value = true;
}, { immediate: true });

const current = computed<IrisPair>(() => ({ left: left.value, right: odd.value ? right.value : left.value }));
const canControl = computed(() => session.canControl);

function wear(pair: IrisPair, remember: boolean): void {
  left.value = pair.left;
  right.value = pair.right;
  odd.value = pair.left !== pair.right;
  eye.setIris(pair.left, pair.right);
  if (remember) {
    book.value = rememberPair(book.value, pair);
    saveIrisBook(session.deviceKey, book.value);
  }
}
function onInput(side: 'left' | 'right', event: Event): void {
  const hex = normalizeHex((event.target as HTMLInputElement).value);
  if (!hex) return;
  dragging.value = true;
  if (side === 'left') left.value = hex; else right.value = hex;
  eye.setIris(current.value.left, current.value.right);
}
/** The picker closed: this is a colour that was chosen, not one passed over. */
function onCommit(): void {
  dragging.value = false;
  wear(current.value, true);
}
function toggleOdd(): void {
  odd.value = !odd.value;
  if (!odd.value) wear({ left: left.value, right: left.value }, true);
}
function swap(): void {
  wear({ left: right.value, right: left.value }, true);
}
async function keep(): Promise<void> {
  const name = await dialog.prompt('给这组瞳色起个名字', { title: '加入我的瞳色集', confirmText: '保存' });
  if (!name?.trim()) return;
  const pair = current.value;
  book.value = { ...book.value, mine: [{ name: name.trim().slice(0, 12), ...pair }, ...book.value.mine.filter(p => pairKey(p) !== pairKey(pair))].slice(0, 24) };
  saveIrisBook(session.deviceKey, book.value);
}
function forget(pair: IrisPair): void {
  book.value = { ...book.value, mine: book.value.mine.filter(p => pairKey(p) !== pairKey(pair)) };
  saveIrisBook(session.deviceKey, book.value);
}
function clearHistory(): void {
  book.value = { ...book.value, recent: [] };
  saveIrisBook(session.deviceKey, book.value);
}
const swatch = (pair: IrisPair) => pair.left === pair.right
  ? { background: pair.left }
  : { background: `linear-gradient(90deg, ${pair.left} 0 50%, ${pair.right} 50% 100%)` };
const isWorn = (pair: IrisPair) => pairKey(pair) === pairKey(current.value);
</script>

<template>
  <MdCard title="瞳色">
    <div class="wells">
      <label class="well">
        <input type="color" :value="left" :disabled="!canControl" :aria-label="odd ? '左眼瞳色' : '双眼瞳色'" @input="onInput('left', $event)" @change="onCommit" />
        <span class="well-text"><strong>{{ odd ? '左眼' : '双眼' }}</strong><code>{{ left }}</code></span>
      </label>
      <button v-if="odd" class="swap" type="button" :disabled="!canControl" aria-label="左右互换" title="左右互换" @click="swap"><UiIcon name="swap_horiz" :size="18" /></button>
      <label v-if="odd" class="well">
        <input type="color" :value="right" :disabled="!canControl" aria-label="右眼瞳色" @input="onInput('right', $event)" @change="onCommit" />
        <span class="well-text"><strong>右眼</strong><code>{{ right }}</code></span>
      </label>
    </div>
    <div class="actions">
      <MdButton variant="tonal" :disabled="!canControl" @click="toggleOdd">{{ odd ? '改回同色' : '异瞳' }}</MdButton>
      <MdButton variant="text" :disabled="!canControl" @click="keep">加入我的瞳色集</MdButton>
      <MdButton variant="text" :disabled="!canControl || isWorn({ left: DEFAULT_IRIS, right: DEFAULT_IRIS })" @click="wear({ left: DEFAULT_IRIS, right: DEFAULT_IRIS }, true)">恢复默认</MdButton>
    </div>

    <p class="group">瞳色集</p>
    <div class="swatches">
      <button v-for="set in BUILTIN_IRIS_SETS" :key="set.name" class="swatch" type="button" :class="{ on: isWorn(set) }" :disabled="!canControl" :title="set.name" @click="wear(set, true)">
        <span class="dot" :style="swatch(set)" /><span class="name">{{ set.name }}</span>
      </button>
    </div>

    <template v-if="book.mine.length">
      <p class="group">我的瞳色集</p>
      <div class="swatches">
        <span v-for="set in book.mine" :key="pairKey(set) + set.name" class="mine">
          <button class="swatch" type="button" :class="{ on: isWorn(set) }" :disabled="!canControl" :title="set.name" @click="wear(set, true)">
            <span class="dot" :style="swatch(set)" /><span class="name">{{ set.name }}</span>
          </button>
          <button class="remove" type="button" :aria-label="`移除 ${set.name}`" :title="`移除 ${set.name}`" @click="forget(set)"><UiIcon name="close" :size="14" /></button>
        </span>
      </div>
    </template>

    <template v-if="book.recent.length">
      <p class="group">最近用过 <button class="link" type="button" @click="clearHistory">清空</button></p>
      <div class="swatches recent">
        <button v-for="pair in book.recent" :key="pairKey(pair)" class="swatch bare" type="button" :class="{ on: isWorn(pair) }" :disabled="!canControl"
          :title="pair.left === pair.right ? pair.left : `${pair.left} / ${pair.right}`" :aria-label="pair.left === pair.right ? pair.left : `${pair.left} 与 ${pair.right}`" @click="wear(pair, true)">
          <span class="dot" :style="swatch(pair)" />
        </button>
      </div>
    </template>
    <p class="hint">瞳色由设备保存并立即生效；瞳色集与历史保存在这台浏览器里。</p>
  </MdCard>
</template>

<style scoped>
.wells { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.well { display: flex; align-items: center; gap: 10px; min-height: 48px; cursor: pointer; }
.well input[type='color'] {
  width: 44px; height: 44px; padding: 0; border: 2px solid var(--md-outline-variant); border-radius: 50%;
  background: none; cursor: pointer; overflow: hidden; flex: none;
}
.well input[type='color']::-webkit-color-swatch-wrapper { padding: 0; }
.well input[type='color']::-webkit-color-swatch { border: 0; border-radius: 50%; }
.well input[type='color']::-moz-color-swatch { border: 0; border-radius: 50%; }
.well input[type='color']:disabled { cursor: not-allowed; opacity: 0.5; }
.well-text { display: flex; flex-direction: column; line-height: 1.25; }
.well-text strong { font: 600 13.5px var(--font-body); }
.well-text code { font-size: 12px; color: var(--md-on-surface-variant); text-transform: uppercase; }
.swap, .remove {
  border: 0; background: transparent; color: var(--md-on-surface-variant); cursor: pointer;
  display: grid; place-items: center; border-radius: 50%;
}
.swap { width: 36px; height: 36px; }
.swap:hover:not(:disabled), .remove:hover { background: var(--md-surface-container-highest); color: var(--md-on-surface); }
.actions { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 10px; }
.group { margin: 16px 0 8px; font: 600 12.5px var(--font-body); color: var(--md-on-surface-variant); display: flex; align-items: center; gap: 8px; }
.link { border: 0; background: none; padding: 0; color: var(--md-primary); font: inherit; cursor: pointer; }
.swatches { display: flex; flex-wrap: wrap; gap: 8px; }
.swatch {
  display: inline-flex; align-items: center; gap: 8px; min-height: 40px; padding: 4px 12px 4px 6px;
  border: 1px solid var(--md-outline-variant); border-radius: 999px; background: transparent;
  color: var(--md-on-surface); font: 500 13px var(--font-body); cursor: pointer;
}
.swatch.bare { padding: 4px; border-radius: 50%; min-width: 40px; justify-content: center; }
.swatch:hover:not(:disabled) { background: var(--md-surface-container-highest); }
.swatch.on { border-color: var(--md-primary); box-shadow: 0 0 0 1px var(--md-primary); }
.swatch:disabled { opacity: 0.5; cursor: not-allowed; }
.dot { width: 26px; height: 26px; border-radius: 50%; flex: none; box-shadow: inset 0 0 0 1px rgba(0, 0, 0, 0.18); }
.mine { display: inline-flex; align-items: center; }
.remove { width: 24px; height: 24px; margin-left: -2px; opacity: 0; }
.mine:hover .remove, .remove:focus-visible { opacity: 1; }
@media (hover: none) { .remove { opacity: 1; } }
.hint { margin: 14px 0 0; font-size: 12px; line-height: 1.5; color: var(--md-on-surface-variant); }
</style>
