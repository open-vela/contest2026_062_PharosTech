<script setup lang="ts">
/* Calendar records are owned by Core; the month grid and editor are local views. */
import { computed, ref } from 'vue';
import { BottomSheet, MdButton, MdTextField, NkActionBar, NkListSection, NkRow, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeScene } from '../../../composables/useEyeScene';
import { calendarScene } from '../../../composables/eyeScenePayload';
import { useProductRecords } from '../../../composables/useProductRecords';
import type { ProductRecord } from '../../../stores/product';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type: string; ff: FormFactor }>();

interface CalRecord extends ProductRecord { id:string; title:string; start_at:number }
interface CalEvent { id: string; title: string; date: string /* YYYY-MM-DD */ }
const core = useProductRecords<CalRecord>('calendar');
const iso = (d: Date): string => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
const today = new Date();
const todayIso = iso(today);

const events = computed<CalEvent[]>(() => core.items.value.map(e => ({id:e.id,title:e.title,date:iso(new Date(e.start_at))})));

/* Month navigation + grid. */
const viewYear = ref(today.getFullYear());
const viewMonth = ref(today.getMonth());
const selected = ref(todayIso);
const WEEK = ['一', '二', '三', '四', '五', '六', '日'];
const monthTitle = computed(() => `${viewYear.value} 年 ${viewMonth.value + 1} 月`);
interface Cell { iso: string; day: number; inMonth: boolean; isToday: boolean; hasEvent: boolean }
const cells = computed<Cell[]>(() => {
  const first = new Date(viewYear.value, viewMonth.value, 1);
  const lead = (first.getDay() + 6) % 7; // Monday-first
  const start = new Date(viewYear.value, viewMonth.value, 1 - lead);
  const set = new Set(events.value.map((e) => e.date));
  return Array.from({ length: 42 }, (_, i) => {
    const d = new Date(start.getFullYear(), start.getMonth(), start.getDate() + i);
    const s = iso(d);
    return { iso: s, day: d.getDate(), inMonth: d.getMonth() === viewMonth.value, isToday: s === todayIso, hasEvent: set.has(s) };
  });
});
function shiftMonth(delta: number): void {
  const d = new Date(viewYear.value, viewMonth.value + delta, 1);
  viewYear.value = d.getFullYear();
  viewMonth.value = d.getMonth();
}
function goToday(): void {
  viewYear.value = today.getFullYear();
  viewMonth.value = today.getMonth();
  selected.value = todayIso;
}

/* Derived lists. */
const daysLeft = (date: string): number => Math.round((new Date(date + 'T00:00:00').getTime() - new Date(todayIso + 'T00:00:00').getTime()) / 86400000);
const dayEvents = computed(() => events.value.filter((e) => e.date === selected.value));
const upcoming = computed(() => events.value.filter((e) => daysLeft(e.date) >= 0).sort((a, b) => a.date.localeCompare(b.date)));
const selectedText = computed(() => {
  const d = new Date(selected.value + 'T00:00:00');
  const rel = daysLeft(selected.value);
  const tag = rel === 0 ? '今天' : rel === 1 ? '明天' : rel === -1 ? '昨天' : '';
  return `${d.getMonth() + 1} 月 ${d.getDate()} 日 周${WEEK[(d.getDay() + 6) % 7]}${tag ? ' · ' + tag : ''}`;
});
const countdownText = (n: number): string => (n === 0 ? '就是今天' : `还有 ${n} 天`);
const subtitle = computed(() => (upcoming.value[0] ? `${upcoming.value[0].title} ${countdownText(daysLeft(upcoming.value[0].date))}` : '暂无即将到来的日程'));

/* Eye link: the nearest upcoming event; edits reach the eyes by themselves. */
const scene = useEyeScene(props.type, () => {
  const n = upcoming.value[0];
  const record = n ? core.items.value.find((r) => r.id === n.id) : undefined;
  return record ? calendarScene({ title: record.title, startAt: record.start_at }) : null;
}, { hideWhenEmpty: true });
const onEyes = computed(() => scene.shown.value || scene.held.value);

/* Add-event editor. */
const editing = ref(false);
const editId = ref<string|null>(null);
const editRevision = ref(0);
const eTitle = ref('');
const eDate = ref(todayIso);
const sheetMode = computed(() => props.ff !== 'desktop');
function openAdd(): void {
  editId.value = null;
  eTitle.value = '';
  eDate.value = selected.value;
  editing.value = true;
}
function openEdit(event: CalEvent): void {
  editRevision.value = core.revision.value;
  editId.value = event.id;
  eTitle.value = event.title;
  eDate.value = event.date;
  editing.value = true;
}
async function save(): Promise<void> {
  const t = eTitle.value.trim();
  if (!t || !/^\d{4}-\d{2}-\d{2}$/.test(eDate.value)) return;
  const start = new Date(eDate.value + 'T00:00:00');
  if (!Number.isFinite(start.getTime()) || iso(start) !== eDate.value) return;
  const saved = await core.mutate(editId.value ? 'update' : 'create', {id:editId.value,record:{title:t,start_at:start.getTime()}}, editId.value ? editRevision.value : undefined);
  if (saved) {
    selected.value = eDate.value;
    editing.value = false;
  }
}
async function remove(id: string): Promise<void> {
  await core.mutate('delete', {id});
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="calendar" title="日历" :subtitle="subtitle" :tone="upcoming.length ? 'ok' : 'default'">
      <MdButton variant="tonal" :disabled="!core.available.value || core.busy.value" @click="openAdd"><UiIcon name="add" :size="18" /> 添加</MdButton>
    </NkHeader>
    <p v-if="!core.available.value || core.error.value" role="status" class="muted">{{ core.error.value || '未连接支持此功能的 Core' }}</p>
    <div class="grid">
      <section class="card">
        <div class="month-bar">
          <MdButton variant="icon" @click="shiftMonth(-1)"><UiIcon name="chevron_left" :size="22" /></MdButton>
          <button class="month-title" type="button" @click="goToday">{{ monthTitle }}</button>
          <MdButton variant="icon" @click="shiftMonth(1)"><UiIcon name="chevron_right" :size="22" /></MdButton>
        </div>
        <div class="week">
          <span v-for="w in WEEK" :key="w" class="muted">{{ w }}</span>
        </div>
        <div class="days">
          <button
            v-for="c in cells"
            :key="c.iso"
            type="button"
            class="day"
            :class="{ out: !c.inMonth, today: c.isToday, sel: c.iso === selected }"
            @click="selected = c.iso"
          >
            <span class="num">{{ c.day }}</span>
            <span v-if="c.hasEvent" class="dot" />
          </button>
        </div>
      </section>

      <div class="stack-col">
        <section class="card">
          <NkListSection :title="selectedText" :card="false">
            <p v-if="!dayEvents.length" class="muted empty">这天没有日程</p>
            <NkRow v-for="e in dayEvents" :key="e.id" icon="calendar" :title="e.title" :sub="countdownText(daysLeft(e.date))">
              <MdButton variant="text" :disabled="core.busy.value" @click="openEdit(e)">编辑</MdButton>
              <MdButton variant="icon" :disabled="core.busy.value" @click="remove(e.id)"><UiIcon name="delete" :size="20" /></MdButton>
            </NkRow>
            <template #trailing>
              <MdButton variant="text" @click="openAdd">添加</MdButton>
            </template>
          </NkListSection>
        </section>

        <section class="card">
          <h3 class="section-title">倒数日</h3>
          <p v-if="!upcoming.length" class="muted empty">添加一个值得期待的日子</p>
          <div class="eye-row">
            <span class="muted">{{ onEyes ? '猫眼正在显示最近的日程' : '把最近的日程显示到猫眼' }}</span>
            <EyeShowButton :shown="onEyes" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
          </div>
          <div class="count-grid">
            <div v-for="e in upcoming.slice(0, 4)" :key="e.id" class="count-card" :class="{ soon: daysLeft(e.date) <= 3 }" @click="selected = e.date">
              <span class="count-title">{{ e.title }}</span>
              <span class="count-num">{{ daysLeft(e.date) }}</span>
              <span class="count-unit muted">{{ daysLeft(e.date) === 0 ? '今天' : '天后' }} · {{ e.date.slice(5).replace('-', '/') }}</span>
            </div>
          </div>
        </section>
      </div>

      <section v-if="!sheetMode && editing" class="card editor">
        <h3 class="section-title">{{ editId ? '编辑日程' : '添加日程' }}</h3>
        <MdTextField v-model="eTitle" label="标题" placeholder="如：体检、纪念日" />
        <MdTextField v-model="eDate" label="日期" type="date" />
        <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!eTitle.trim() || core.busy.value || !core.available.value" @primary="save" @secondary="editing = false" />
      </section>
    </div>

    <BottomSheet v-if="sheetMode" :open="editing" :title="editId ? '编辑日程' : '添加日程'" @close="editing = false">
      <div class="sheet-body">
        <MdTextField v-model="eTitle" label="标题" placeholder="如：体检、纪念日" />
        <MdTextField v-model="eDate" label="日期" type="date" />
        <NkActionBar primary-text="保存" primary-icon="check" secondary-text="取消" secondary-icon="close" :disabled="!eTitle.trim() || core.busy.value || !core.available.value" @primary="save" @secondary="editing = false" />
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid { grid-template-columns: 1.2fr 1fr; }
.feature.desktop .grid > .editor { grid-column: 1 / -1; }
.stack-col { display: flex; flex-direction: column; gap: 16px; }
.eye-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.card {
  display: flex; flex-direction: column; gap: 12px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.empty { padding: 12px 0; text-align: center; }
.month-bar { display: flex; align-items: center; justify-content: space-between; }
.month-title {
  min-height: 44px; padding: 0 12px; border: 0; border-radius: var(--radius-m);
  background: transparent; color: inherit; font: inherit; font-size: 17px; font-weight: 600; cursor: pointer;
}
.month-title:hover { background: var(--md-surface-container-high); }
.week, .days { display: grid; grid-template-columns: repeat(7, 1fr); gap: 4px; }
.week span { text-align: center; font-size: 12px; padding: 4px 0; }
.day {
  position: relative; display: flex; flex-direction: column; align-items: center; justify-content: center;
  min-height: 44px; aspect-ratio: 1; border: 0; border-radius: var(--radius-m);
  background: transparent; color: inherit; font: inherit; cursor: pointer;
  transition: background var(--dur-short, 150ms) var(--ease-standard, ease);
}
.feature.desktop .day { aspect-ratio: auto; min-height: 52px; }
.day:hover { background: var(--md-surface-container-high); }
.day.out { color: var(--md-outline); }
.day.today .num { color: var(--md-primary); font-weight: 700; }
.day.sel { background: var(--md-primary); color: var(--md-on-primary); }
.day.sel .num { color: inherit; }
.dot { position: absolute; bottom: 6px; width: 5px; height: 5px; border-radius: 50%; background: var(--md-primary); }
.day.sel .dot { background: var(--md-on-primary); }
.count-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
.count-card {
  display: flex; flex-direction: column; gap: 2px; min-height: 44px; padding: 14px;
  border-radius: var(--radius-m); background: var(--md-surface-container-high); cursor: pointer;
}
.count-card.soon { background: var(--md-primary-container); color: var(--md-on-primary-container); }
.count-title { font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.count-num { font-size: 36px; font-weight: 700; line-height: 1.1; font-variant-numeric: tabular-nums; }
.count-unit { font-size: 12px; }
.sheet-body { display: flex; flex-direction: column; gap: 16px; padding: 4px 0 8px; }
</style>
