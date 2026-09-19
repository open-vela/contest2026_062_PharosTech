<script setup lang="ts">
/* Alarm feature. Local list of alarms (persisted), each row shows a large
 * time, repeat days and label with an enable toggle. Editing happens in a
 * side panel (desktop) or BottomSheet (phone/tablet) with NkTimeWheel,
 * repeat-day chips and a label field. Alarms are Core records. The next alarm
 * can be put on the eyes; a ringing one goes there by itself (useAlarms). */
import { computed, ref } from 'vue';
import { BottomSheet, MdButton, MdSwitch, MdTextField, NkActionBar, NkChipSelect, NkListSection, NkRow, NkTimeWheel, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useAlarms, type Alarm } from '../../../composables/useAlarms';
import type { FormFactor } from '../../../composables/useFormFactor';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type: string; ff: FormFactor }>();
const { product, alarms, sorted, nextAlarm, ringing, scene, dismiss, snooze } = useAlarms();
const onEyes = computed(() => scene.shown.value || scene.held.value);
const DAYS = [
  { id: 'mon', label: '一' }, { id: 'tue', label: '二' }, { id: 'wed', label: '三' }, { id: 'thu', label: '四' },
  { id: 'fri', label: '五' }, { id: 'sat', label: '六' }, { id: 'sun', label: '日' },
];
const DAY_ORDER = DAYS.map((d) => d.id);
const DAY_NAME: Record<string, string> = Object.fromEntries(DAYS.map((d) => [d.id, d.label]));

const eOffset = ref(-new Date().getTimezoneOffset());
const editRevision = ref(0);

/* Editor state. */
const editing = ref(false);
const editId = ref<string | null>(null);
const eTime = ref('07:00');
const eLabel = ref('');
const eRepeat = ref<string[]>([]);
const sheetMode = computed(() => props.ff !== 'desktop');

function repeatText(r: string[]): string {
  if (!r.length) return '仅一次';
  if (r.length === 7) return '每天';
  const wk = ['mon', 'tue', 'wed', 'thu', 'fri'];
  if (r.length === 5 && wk.every((d) => r.includes(d))) return '工作日';
  if (r.length === 2 && r.includes('sat') && r.includes('sun')) return '周末';
  return '周' + [...r].sort((a, b) => DAY_ORDER.indexOf(a) - DAY_ORDER.indexOf(b)).map((d) => DAY_NAME[d]).join('、');
}
const subtitle = computed(() => !product.available ? '请连接支持 Core 的设备' : nextAlarm.value
  ? `下次 ${new Date(nextAlarm.value.next_at).toLocaleString()}（浏览器时区）` : '没有待触发的闹钟');

function openNew(): void {
  editRevision.value = product.records.alarm?.revision ?? 0;
  eOffset.value = -new Date().getTimezoneOffset();
  editId.value = null;
  eTime.value = '07:00';
  eLabel.value = '';
  eRepeat.value = [];
  editing.value = true;
}
function openEdit(a: Alarm): void {
  editRevision.value = product.records.alarm?.revision ?? 0;
  eOffset.value = a.utc_offset_minutes;
  editId.value = a.id;
  eTime.value = a.time;
  eLabel.value = a.label;
  eRepeat.value = [...a.repeat];
  editing.value = true;
}
function onRepeat(v: string | string[]): void {
  eRepeat.value = Array.isArray(v) ? v : [v];
}
async function save(): Promise<void> {
  if (await product.mutate('alarm', editId.value ? 'update' : 'create', {
    id: editId.value, record: { time:eTime.value, label:eLabel.value.trim(),
      repeat:[...eRepeat.value], enabled:true, utc_offset_minutes:eOffset.value },
  }, editRevision.value)) editing.value = false;
}
async function remove(): Promise<void> {
  if (editId.value && await product.mutate('alarm', 'delete', { id:editId.value }, editRevision.value)) editing.value = false;
}
function toggle(a: Alarm, v: boolean): void {
  void product.mutate('alarm', 'update', { id:a.id, record:{ ...a, enabled:v } });
}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="alarm" title="闹钟" :subtitle="subtitle" :tone="nextAlarm ? 'ok' : 'default'">
      <MdButton variant="tonal" @click="openNew"><UiIcon name="add" :size="18" /> 新建</MdButton>
    </NkHeader>
    <p v-if="product.errors.alarm" role="alert">{{ product.errors.alarm }}</p>
    <p class="muted">闹钟由设备运行，关闭网页仍有效；设备关机或时钟未校准时不能准时提醒。固定 UTC 偏移不自动跟随夏令时。</p>
    <div v-for="a in ringing" :key="a.id" class="card ring" role="alert">
      <strong>闹钟到点：{{ a.time }}{{ a.label ? ' · ' + a.label : '' }}</strong>
      <div class="ring-actions">
        <MdButton :disabled="product.busy.alarm" @click="dismiss(a)">关闭提醒</MdButton>
        <MdButton variant="tonal" :disabled="product.busy.alarm" @click="snooze(a)">5 分钟后提醒</MdButton>
      </div>
    </div>
    <div class="eye-row">
      <span class="muted">{{ onEyes ? '猫眼正在显示闹钟' : nextAlarm ? `可将下一个闹钟（${nextAlarm.time}）显示到猫眼` : '没有待触发的闹钟可显示' }}</span>
      <EyeShowButton :shown="onEyes" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
    </div>
    <div class="grid" :class="{ editing: editing && !sheetMode }">
      <section class="card">
        <NkListSection title="我的闹钟" :card="false">
          <p v-if="!alarms.length" class="muted empty">还没有闹钟，点右上角「新建」</p>
          <div v-for="a in sorted" :key="a.id" class="alarm-row" :class="{ off: !a.enabled }">
            <NkRow :title="a.time" :sub="(a.label ? a.label + ' · ' : '') + repeatText(a.repeat) + ' · UTC偏移 ' + a.utc_offset_minutes + ' 分钟' + (a.status === 'missed' ? ' · 已错过' : '')" tappable @tap="openEdit(a)">
              <span class="switch-wrap" @click.stop>
                <MdSwitch :model-value="a.enabled" @update:model-value="toggle(a, $event)" />
              </span>
            </NkRow>
          </div>
        </NkListSection>
      </section>

      <!-- Desktop: inline side panel editor. -->
      <section v-if="!sheetMode && editing" class="card editor">
        <h3 class="section-title">{{ editId ? '编辑闹钟' : '新建闹钟' }}</h3>
        <NkTimeWheel v-model="eTime" />
        <NkChipSelect :model-value="eRepeat" :options="DAYS" multi label="重复" @update:model-value="onRepeat" />
        <MdTextField v-model="eLabel" label="标签" placeholder="如：起床、吃药" />
        <NkActionBar primary-text="保存" primary-icon="check" :secondary-text="editId ? '删除' : '取消'" :secondary-icon="editId ? 'delete' : 'close'" @primary="save" @secondary="editId ? remove() : (editing = false)" />
      </section>
    </div>

    <!-- Phone / tablet: bottom sheet editor. -->
    <BottomSheet v-if="sheetMode" :open="editing" :title="editId ? '编辑闹钟' : '新建闹钟'" @close="editing = false">
      <div class="sheet-body">
        <NkTimeWheel v-model="eTime" />
        <NkChipSelect :model-value="eRepeat" :options="DAYS" multi label="重复" @update:model-value="onRepeat" />
        <MdTextField v-model="eLabel" label="标签" placeholder="如：起床、吃药" />
        <NkActionBar primary-text="保存" primary-icon="check" :secondary-text="editId ? '删除' : '取消'" :secondary-icon="editId ? 'delete' : 'close'" @primary="save" @secondary="editId ? remove() : (editing = false)" />
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid.editing { grid-template-columns: 1fr 360px; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.editor { position: sticky; top: 0; align-self: start; }
.empty { padding: 12px 0; text-align: center; }
/* Large tabular time as the row title; dim when disabled. */
.ring { border: 1px solid rgba(var(--md-primary-rgb), 0.45); }
.ring-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.eye-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; flex-wrap: wrap; }
.alarm-row :deep(.nk-row-title) { font-size: 30px; font-weight: 700; font-variant-numeric: tabular-nums; line-height: 1.15; }
.alarm-row.off :deep(.nk-row-title) { color: var(--md-on-surface-variant); }
.switch-wrap { display: inline-flex; align-items: center; min-height: 44px; }
.sheet-body { display: flex; flex-direction: column; gap: 16px; padding: 4px 0 8px; }
</style>
