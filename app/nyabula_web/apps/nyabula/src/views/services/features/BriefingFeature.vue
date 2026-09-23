<script setup lang="ts">
import { computed, reactive, watch } from 'vue';
import { NkActionBar, NkListSection, NkProgressRing, NkSegmentRow, NkToggleRow } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useBriefingStore } from '../../../stores/briefing';
import type { FormFactor } from '../../../composables/useFormFactor';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { briefingSource as source, useBriefingScene } from './sceneLinks';
import EyeShowButton from './EyeShowButton.vue';

const props = defineProps<{ type:string; ff:FormFactor }>();
const briefing = useBriefingStore();
const schedule = reactive({ schedule_enabled:false, morning_minute:480, evening_minute:1200,
  utc_offset_minutes:-new Date().getTimezoneOffset() });
let syncing = false;
/* The index only moves while the device plays the briefing: poll fast then,
 * slowly otherwise, and never while the tab is hidden. */
let skipped = 0;
useVisiblePoll(() => {
  if (briefing.state && !briefing.state.playing && ++skipped % 5) return;
  void briefing.refresh();
}, 2000);
const scene = useBriefingScene(props.type);
const onEyes = computed(() => scene.shown.value || scene.held.value);
watch(() => briefing.state, state => { if (state && !syncing) Object.assign(schedule, state); });
let saveTimer:ReturnType<typeof setTimeout>|undefined;
watch(schedule, () => {
  if (!briefing.state) return;
  clearTimeout(saveTimer);
  saveTimer=setTimeout(async () => { syncing=true; await briefing.act('configure', { ...schedule }); syncing=false; }, 350);
}, { deep:true });
const items = computed(() => briefing.state?.items ?? []);
const current = computed(() => items.value[briefing.state?.index ?? 0] ?? null);
const subtitle = computed(() => !briefing.available ? '请连接支持简报的 Core 设备'
  : briefing.state?.generated_at ? `生成于 ${new Date(briefing.state.generated_at).toLocaleString()}` : '尚未生成');
const morningModel = computed({ get:() => String(schedule.morning_minute), set:(value:string) => { schedule.morning_minute=Number(value); } });
const eveningModel = computed({ get:() => String(schedule.evening_minute), set:(value:string) => { schedule.evening_minute=Number(value); } });
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="article" title="今日简报" :subtitle="subtitle" :tone="briefing.state?.playing ? 'ok' : 'default'" />
    <p v-if="briefing.error" role="alert">{{ briefing.error }}</p>
    <p class="muted">简报由设备上的天气缓存、未来 24 小时日程、未完成待办和用户明确保存的记忆生成。内容均标明来源；不会把模型推测写成事实。</p>
    <div class="grid">
      <section class="card now">
        <NkProgressRing :value="items.length ? ((briefing.state?.index ?? 0) + 1) / items.length : 0" :size="ff === 'phone' ? 180 : 220" :stroke="12" tone="primary">
          <div><strong class="big">{{ items.length ? (briefing.state?.index ?? 0) + 1 : 0 }}</strong><span> / {{ items.length }}</span></div>
        </NkProgressRing>
        <article v-if="current"><h3>{{ current.title }}</h3><p>{{ current.text }}</p><small>来源：{{ source(current.source) }}</small></article>
        <p v-else>点击“重新生成”建立一份设备简报。</p>
        <NkActionBar :primary-text="briefing.state?.playing ? '停止显示' : '逐条显示到猫眼'"
          :primary-icon="briefing.state?.playing ? 'stop' : 'play_arrow'" secondary-text="下一条" secondary-icon="skip_next"
          :disabled="!items.length || briefing.busy" @primary="briefing.act(briefing.state?.playing ? 'stop' : 'start')" @secondary="briefing.act('next')" />
        <EyeShowButton kind="wide" :shown="onEyes" :disabled="!scene.canShow.value" @toggle="scene.toggle()" />
        <p class="muted">「逐条显示」由设备依次播报各条标题；「在设备上显示」把简报进度卡留在猫眼上，并随当前条目更新。</p>
      </section>
      <section class="card">
        <NkListSection title="简报条目" :card="false">
          <article v-for="(item, index) in items" :key="item.id" class="item" :class="{ current:index === briefing.state?.index }">
            <strong>{{ item.title }}</strong><p>{{ item.text }}</p><small>来源：{{ source(item.source) }}</small>
          </article>
        </NkListSection>
        <NkActionBar primary-text="重新生成" primary-icon="refresh" :disabled="briefing.busy || !briefing.available" @primary="briefing.act('generate')" />
        <NkToggleRow v-model="schedule.schedule_enabled" title="定时简报" sub="由设备生成并逐条显示；关闭网页仍有效" />
        <NkSegmentRow v-model="morningModel" title="早间简报" :items="[
          {id:'420',label:'07:00'},{id:'480',label:'08:00'},{id:'540',label:'09:00'}]" />
        <NkSegmentRow v-model="eveningModel" title="晚间简报" :items="[
          {id:'1140',label:'19:00'},{id:'1200',label:'20:00'},{id:'1260',label:'21:00'}]" />
        <p class="muted">定时按固定 UTC 偏移 {{ schedule.utc_offset_minutes }} 分钟计算；跨夏令时地区需手动更新。</p>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature,.card { display:flex; flex-direction:column; gap:16px; }.grid { display:grid; gap:16px; }.desktop .grid,.tablet .grid { grid-template-columns:1fr 1.25fr; }
.card { padding:16px; border-radius:var(--radius-l); background:var(--md-surface-container); }.now { align-items:center; text-align:center; }
.item { padding:12px; border-radius:var(--radius-m); background:var(--md-surface-container-high); }.item.current { background:var(--md-secondary-container); }
.big { font-size:40px; }h3,p { margin:0; }small,.muted { color:var(--md-on-surface-variant); }
</style>
