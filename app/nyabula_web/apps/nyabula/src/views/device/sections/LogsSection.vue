<script setup lang="ts">
/* Logs: logs.tail polled every 2 s while "实时" is on, the tab is visible and
 * the link is up. Keeps the newest MAX_LINES lines, follows the tail unless
 * the reader scrolled up. ENOTFOUND (older firmware) ends in an empty state
 * and stops the polling for good. */
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { EmptyState, MdButton, MdCard, MdSwitch, Skeleton, UiIcon, useToastStore } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';
import { copyText } from '../../../lib/clipboard';
import { isUnsupportedError, mergeLogLines, parseLogsTail, type LogLine } from '../../../lib/deviceMaint';

const POLL_MS = 2000;
/** After a failed read: slower, and never a toast (it would repeat forever). */
const RETRY_MS = 5000;
/** A full page means more is waiting: fetch the rest right away. */
const CATCH_UP_MS = 150;
const PAGE = 200;
const MAX_LINES = 1000;
/** Distance from the bottom (px) that still counts as "following the tail". */
const STICK_PX = 24;

const session = useSessionStore();
const toast = useToastStore();

const lines = ref<LogLine[]>([]);
const live = ref(true);
const loaded = ref(false);
const unsupported = ref(false);
const error = ref<string | null>(null);
const following = ref(true);
const viewport = ref<HTMLElement | null>(null);

let cursor: number | null = null;
let timer: ReturnType<typeof setTimeout> | undefined;
let inFlight = false;
let alive = true;

const canPoll = (): boolean => alive && live.value && !unsupported.value && session.connected && !document.hidden;
const statusText = computed(() => (!session.connected ? '未连接' : live.value ? '实时' : '已暂停'));

function schedule(ms: number): void {
  clearTimeout(timer);
  timer = undefined;
  if (canPoll()) timer = setTimeout(() => void poll(), ms);
}

async function poll(): Promise<void> {
  if (inFlight || !canPoll()) return;
  inFlight = true;
  let delay = POLL_MS;
  try {
    const data: Record<string, unknown> = { limit: PAGE };
    if (cursor !== null) data.after = cursor;
    const tail = parseLogsTail(await session.request('logs.tail', data));
    if (!alive) return;
    // A cursor that moved backwards means the device rebooted and renumbered.
    const restarted = cursor !== null && tail.next !== null && tail.next < cursor;
    lines.value = mergeLogLines(lines.value, tail, MAX_LINES, restarted);
    if (tail.next !== null) cursor = tail.next;
    loaded.value = true;
    error.value = null;
    if (tail.lines.length >= PAGE) delay = CATCH_UP_MS;
    if (following.value) void nextTick(scrollToEnd);
  } catch (e) {
    if (!alive) return;
    if (isUnsupportedError(e)) unsupported.value = true;
    else {
      error.value = e instanceof Error ? e.message : String(e);
      delay = RETRY_MS;
    }
    loaded.value = true;
  } finally {
    inFlight = false;
    schedule(delay);
  }
}

function scrollToEnd(): void {
  const el = viewport.value;
  if (el) el.scrollTop = el.scrollHeight;
}
function onScroll(): void {
  const el = viewport.value;
  if (el) following.value = el.scrollHeight - el.scrollTop - el.clientHeight <= STICK_PX;
}
function jumpToEnd(): void {
  following.value = true;
  scrollToEnd();
}
/** Clears the view only: the cursor stays, so old lines do not come back. */
function clearView(): void {
  lines.value = [];
  following.value = true;
}
async function copyAll(): Promise<void> {
  if (!lines.value.length) return;
  const ok = await copyText(lines.value.map((l) => l.text).join('\n'));
  if (ok) toast.ok(`已复制 ${lines.value.length} 行日志`);
  else toast.show('复制失败：浏览器拒绝访问剪贴板，请手动选中文本复制', 'error');
}

function onVisibility(): void {
  if (document.hidden) {
    clearTimeout(timer);
    timer = undefined;
  } else schedule(0);
}
watch(live, (on) => {
  if (on) schedule(0);
  else clearTimeout(timer);
});
watch(() => session.connected, (c, prev) => {
  if (!c) {
    clearTimeout(timer);
    return;
  }
  if (prev === false) unsupported.value = false; // may be another firmware now
  schedule(0);
});
onMounted(() => {
  document.addEventListener('visibilitychange', onVisibility);
  schedule(0);
});
onBeforeUnmount(() => {
  alive = false;
  clearTimeout(timer);
  document.removeEventListener('visibilitychange', onVisibility);
});
</script>

<template>
  <div class="stack">
    <MdCard v-if="unsupported">
      <EmptyState compact icon="terminal" title="此固件不支持" hint="设备固件未提供日志读取（logs.tail），升级固件后可用。" />
    </MdCard>
    <MdCard v-else>
      <div class="toolbar">
        <div class="row live">
          <MdSwitch v-model="live" :disabled="!session.connected" label="实时" />
          <span class="tag" :class="session.connected && live ? 'ok' : 'warn'">{{ statusText }}</span>
          <span class="muted count">{{ lines.length }} 行</span>
        </div>
        <div class="row wrap actions">
          <MdButton variant="text" :disabled="!session.connected" @click="live = !live"><UiIcon :name="live ? 'pause' : 'play_arrow'" :size="16" /> {{ live ? '暂停' : '继续' }}</MdButton>
          <MdButton variant="text" :disabled="!lines.length" @click="clearView"><UiIcon name="delete" :size="16" /> 清空视图</MdButton>
          <MdButton variant="tonal" :disabled="!lines.length" @click="copyAll"><UiIcon name="content_copy" :size="16" /> 复制全部</MdButton>
        </div>
      </div>
      <p v-if="error" class="err-line" role="alert"><UiIcon name="error" :size="16" /> 读取日志失败：{{ error }}（{{ live ? '稍后自动重试' : '已暂停' }}）</p>
      <Skeleton v-if="!loaded" :lines="6" />
      <div v-else class="log-wrap">
        <div ref="viewport" class="log mono" role="log" aria-live="off" aria-label="设备日志" tabindex="0" @scroll.passive="onScroll">
          <p v-if="!lines.length" class="log-empty">{{ live ? '等待设备输出日志…' : '视图是空的，打开「实时」继续接收。' }}</p>
          <div v-for="l in lines" :key="l.seq" class="ln" :class="{ mark: l.mark }">{{ l.text || ' ' }}</div>
        </div>
        <button v-if="!following && lines.length" class="to-end" type="button" @click="jumpToEnd"><UiIcon name="expand_more" :size="18" /> 回到最新</button>
      </div>
      <p class="muted hint">每 2 秒读取一次，只保留最近 {{ MAX_LINES }} 行；离开此页或切到后台标签页时停止读取。向上滚动会暂停自动跟随。</p>
    </MdCard>
  </div>
</template>

<style scoped>
.toolbar { display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 8px 12px; margin-bottom: 12px; }
.live { gap: 10px; min-width: 0; }
.count { font-size: 12.5px; white-space: nowrap; }
.actions { gap: 4px; margin-left: auto; }
.actions .md-btn { padding: 8px 14px; font-size: 13px; }
.err-line { display: flex; align-items: center; gap: 6px; margin: 0 0 10px; font-size: 12.5px; color: var(--md-error); }
.err-line .ui-icon { flex: none; }
.log-wrap { position: relative; }
.log {
  height: min(60dvh, 560px);
  min-height: 240px;
  overflow: auto;
  padding: 10px 12px;
  border-radius: var(--radius-m);
  background: var(--md-surface-container-lowest, var(--md-surface));
  border: 1px solid var(--md-outline-variant);
  color: var(--md-on-surface);
  font-size: 12px;
  line-height: 1.55;
  overscroll-behavior: contain;
}
.log:focus-visible { outline: 2px solid var(--md-primary); outline-offset: 1px; }
.ln { white-space: pre-wrap; overflow-wrap: anywhere; }
.ln.mark { color: var(--md-on-surface-variant); text-align: center; padding: 4px 0; }
.log-empty { margin: 0; color: var(--md-on-surface-variant); font-family: var(--font-body); font-size: 13px; }
.to-end {
  position: absolute;
  right: 12px;
  bottom: 12px;
  display: inline-flex;
  align-items: center;
  gap: 4px;
  border: none;
  padding: 6px 12px 6px 8px;
  border-radius: var(--radius-full);
  background: var(--md-secondary-container);
  color: var(--md-on-secondary-container);
  font: 600 12.5px var(--font-body);
  cursor: pointer;
  box-shadow: var(--md-elev-1);
}
.hint { font-size: 12px; margin: 10px 0 0; line-height: 1.6; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
