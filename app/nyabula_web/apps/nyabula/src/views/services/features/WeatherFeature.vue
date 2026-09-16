<script setup lang="ts">
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue';
import { MdButton, MdSwitch, MdTextField, NkWeatherCard } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useWeatherStore, weatherKind } from '../../../stores/weather';
import { useEyeStore } from '../../../stores/eye';
import type { FormFactor } from '../../../composables/useFormFactor';

defineProps<{ type: string; ff: FormFactor }>();
const weather = useWeatherStore();
const eye = useEyeStore();
const form = reactive({ host:'', city:'', province:'', key:'', enabled:true, revision:0 });
const editing = ref(false);
let poll: ReturnType<typeof setInterval> | undefined;
let mounted = true;
onMounted(async () => {
  await weather.refresh();
  if (!mounted) return;
  loadForm(); poll = setInterval(() => void weather.refresh(), 10000);
});
onUnmounted(() => { mounted = false; clearInterval(poll); form.key = ''; });
function loadForm(): void {
  if (weather.status) Object.assign(form, { host:weather.status.host, city:weather.status.city,
    province:weather.status.province, enabled:weather.status.enabled, revision:weather.status.revision, key:'' });
}
function openConfig(): void { loadForm(); editing.value = true; }
async function save(): Promise<void> {
  const { key, ...data } = form;
  if (await weather.configure({ ...data, ...(key ? { key } : {}) })) { form.key = ''; editing.value = false; }
}
function show(): void {
  if (!weather.now.temperature) return;
  void eye.setScene('weather', eye.sceneStyle, { city:weather.now.location?.name,
    temp:Math.round(weather.now.temperature.value), condition:weatherKind(weather.now.condition?.code) });
}
const stale = computed(() => !weather.now.fetched_at || Date.now() - weather.now.fetched_at > 1200000 || !!weather.now.last_error);
const place = computed(() => weather.now.location?.name ?? weather.status?.city ?? '未配置城市');
function date(value?: string | number): string {
  if (!value) return '—';
  return new Date(value).toLocaleString('zh-CN', { timeZone:weather.now.location?.tz || undefined, month:'numeric', day:'numeric', hour:'2-digit', minute:'2-digit' });
}
const attribution = computed(() => Array.from(new Set([weather.now, weather.daily, weather.hourly, weather.alerts]
  .flatMap(section => section.metadata?.attributions ?? []))));
</script>

<template>
  <div class="weather-page" :class="ff">
    <NkHeader icon="cloud" title="天气" :subtitle="weather.available ? place : '请连接支持天气服务的 Core 设备'">
      <MdButton variant="tonal" :disabled="!weather.available" @click="openConfig">配置和风天气</MdButton>
    </NkHeader>
    <p v-if="weather.error" role="alert">{{ weather.error }}</p>
    <p v-if="weather.status && !weather.status.clock_valid" role="alert">设备时钟未校准，自动获取尚未运行。请先在系统设置校准时间。</p>
    <section v-if="editing || (weather.available && !weather.status?.key_set)" class="card config">
      <h3>使用你自己的和风天气账号</h3>
      <MdTextField v-model="form.host" label="API Host" placeholder="控制台 → 设置，例如 xxx.qweatherapi.com" />
      <MdTextField v-model="form.key" type="password" label="API Key" :placeholder="weather.status?.key_set ? '已保存，留空保留原 Key' : '请输入 API Key'" autocomplete="new-password" />
      <MdTextField v-model="form.city" label="城市" placeholder="例如 大连" />
      <MdTextField v-model="form.province" label="省份（可选，用于同名城市）" placeholder="例如 辽宁" />
      <label class="row"><MdSwitch v-model="form.enabled" />每 10 分钟自动获取天气和预警</label>
      <p class="muted">Key 仅提交到所连接设备，不保存在浏览器本地存储，读取配置时不返回。设备端保存凭据；请仅在可信连接上配置。调用消耗你自己的 API 配额。</p>
      <MdButton :disabled="weather.busy || !weather.available" @click="save">保存到设备</MdButton>
    </section>
    <div class="columns">
      <section class="card">
        <NkWeatherCard v-if="weather.now.temperature" :city="place" :temp="Math.round(weather.now.temperature.value)"
          :kind="weatherKind(weather.now.condition?.code)" :high="weather.daily.days?.[0]?.temperatureMax.value"
          :low="weather.daily.days?.[0]?.temperatureMin.value" :extra="weather.now.condition?.text" />
        <p v-else>还没有获取到天气。请先配置账号和城市。</p>
        <p class="muted">{{ stale ? '缓存可能过时 · ' : '' }}获取时间 {{ date(weather.now.fetched_at) }} · 地点时区 {{ weather.now.location?.tz || '未确定' }}</p>
        <p v-if="weather.status?.last_error" role="alert">本轮有请求失败（{{ weather.status.last_error }}），保留上次成功结果，不代表天气或预警已解除。</p>
        <dl v-if="weather.now.temperature" class="metrics">
          <div><dt>体感</dt><dd>{{ weather.now.feelsLike?.value ?? '—' }} °C</dd></div>
          <div><dt>湿度</dt><dd>{{ weather.now.humidity == null ? '—' : Math.round(weather.now.humidity * 100) }}%</dd></div>
          <div><dt>风速</dt><dd>{{ weather.now.wind?.speed.value ?? '—' }} m/s</dd></div>
          <div><dt>气压</dt><dd>{{ weather.now.pressure?.value ?? '—' }} hPa</dd></div>
          <div><dt>能见度</dt><dd>{{ weather.now.visibility?.value ?? '—' }} m</dd></div>
          <div><dt>紫外线</dt><dd>{{ weather.now.uvIndex ?? '—' }}</dd></div>
        </dl>
        <div class="row wrap"><MdButton :disabled="!weather.status?.enabled || weather.status?.refreshing" @click="weather.fetchNow">{{ weather.status?.refreshing ? '正在获取' : '立即获取' }}</MdButton>
          <MdButton variant="tonal" :disabled="!weather.now.temperature" @click="show">简要显示到猫眼</MdButton></div>
      </section>
      <section class="card">
        <h3>天气预警</h3>
        <p class="muted">每 10 分钟检查；首次、更新和取消消息各提醒一次。通知保存在设备，猫眼显示简要标题。以官方发布为准。</p>
        <p v-if="weather.alerts.last_error" role="alert">预警刷新失败（{{ weather.alerts.last_error }}），下方可能是缓存。</p>
        <p v-if="weather.alerts.fetched_at && !weather.alerts.alerts?.length">查询时暂无预警 · {{ date(weather.alerts.fetched_at) }}</p>
        <article v-for="alert in weather.alerts.alerts" :key="alert.id" class="alert-item">
          <strong>{{ alert.messageType.code === 'cancel' ? '已取消 · ' : '' }}{{ alert.headline }}</strong>
          <p>{{ alert.description }}</p><p v-if="alert.instruction">{{ alert.instruction }}</p>
          <small>{{ alert.senderName }} · 有效至 {{ date(alert.expireTime) }}</small>
        </article>
      </section>
    </div>
    <section v-if="weather.daily.days?.length" class="card">
      <h3>未来 7 天天气</h3>
      <div class="forecast"><article v-for="day in weather.daily.days" :key="day.forecastStartTime">
        <strong>{{ date(day.forecastStartTime) }}</strong><p>{{ day.daytime.condition.text }} / {{ day.nighttime.condition.text }}</p>
        <p>{{ day.temperatureMin.value }}–{{ day.temperatureMax.value }} °C</p>
        <small>日出 {{ date(day.astro?.sunrise) }}<br>日落 {{ date(day.astro?.sunset) }}</small>
      </article></div>
    </section>
    <section v-if="weather.hourly.hours?.length" class="card">
      <h3>逐小时预报</h3><div class="forecast"><article v-for="(hour, i) in weather.hourly.hours" :key="i">
        <strong>{{ date(hour.forecastTime || hour.forecastStartTime) }}</strong><p>{{ hour.condition.text }}</p><p>{{ hour.temperature.value }} °C</p>
      </article></div>
    </section>
    <footer class="muted">数据来源：和风天气 QWeather。<template v-for="text in attribution" :key="text">
      <a v-if="text.startsWith('https://')" :href="text" target="_blank" rel="noopener noreferrer">数据归因与许可</a><span v-else>{{ text }}</span> </template>
    </footer>
  </div>
</template>

<style scoped>
.weather-page,.card { display:flex; flex-direction:column; gap:16px; }
.card { padding:20px; border-radius:var(--radius-l); background:var(--md-surface-container); }
.columns { display:grid; gap:16px; grid-template-columns:1fr; }.desktop .columns { grid-template-columns:1.1fr 1fr; }
.row { display:flex; gap:12px; align-items:center; }.wrap { flex-wrap:wrap; }
.metrics { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:16px; margin:0; }
dt,.muted,small { color:var(--md-on-surface-variant); }dd { margin:6px 0 0; font-weight:600; }
.forecast { display:flex; gap:12px; overflow-x:auto; }.forecast article { flex:0 0 155px; padding:12px; border-radius:var(--radius-m); background:var(--md-surface-container-high); }
.alert-item { border-left:3px solid var(--md-error); padding-left:12px; white-space:pre-wrap; }
h3,p { margin:0; }footer { font-size:12px; }.config { max-width:700px; }
</style>
