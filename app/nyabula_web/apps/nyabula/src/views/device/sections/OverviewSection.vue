<script setup lang="ts">
/* Overview: sys.info readout, device rename (owner), demo carousel toggle
 * (family+, not in the device build), disconnect / forget device. */
import { computed, nextTick, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { EmptyState, MdButton, MdCard, MdSwitch, MdTextField, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { batteryOf, fmtUptime, rssiBars, useSysInfo, wifiOf } from './sysinfo';
import { DEVICE_NAME_MAX_BYTES, parseDeviceName, utf8Length, validateDeviceName } from '../../../lib/deviceName';

const router = useRouter();
const dialog = useDialogStore();
const toast = useToastStore();
const { session, task, info } = useSysInfo();

const battery = computed(() => batteryOf(info.value));
const wifi = computed(() => wifiOf(info.value));
const roleZh: Record<string, string> = { owner: '主人 (owner)', family: '家人 (family)', guest: '访客 (guest)' };
const batteryValue = computed(() => (battery.value.external ? '已连接电源' : battery.value.level !== null ? battery.value.level + '%' : '—'));
const batterySub = computed(() => (battery.value.external ? '外接供电，无电池' : battery.value.charging ? '充电中' : battery.value.level !== null ? '使用电池' : '无数据'));
const wifiSub = computed(() => (wifi.value.rssi !== null ? `${wifi.value.rssi} dBm` : '无信号数据'));

/* ---- device name ----
 * session.device.name is the copy every shell reads; sys.info only fills in
 * when hello carried none. `named` (network.status) tells whether the name is
 * still the factory default; older firmware does not report it. */
const deviceName = computed(() => session.device?.name || info.value?.device?.name || null);
const named = ref<boolean | null>(null);
async function loadNamed(): Promise<void> {
  try {
    const r = parseDeviceName(await session.request('network.status'));
    named.value = r.name ? r.named : null;
    if (r.name) session.setDeviceName(r.name);
  } catch {
    named.value = null; // cosmetic only: stay quiet
  }
}
watch(() => session.connected, (c) => { if (c) void loadNamed(); }, { immediate: true });

const editing = ref(false);
const draft = ref('');
const renaming = ref(false);
const nameField = ref<{ focus: () => void } | null>(null);
const draftName = computed(() => draft.value.trim());
const nameError = computed(() => validateDeviceName(draftName.value));
const nameHint = computed(() => `${utf8Length(draftName.value)}/${DEVICE_NAME_MAX_BYTES} 字节${draftName.value ? '' : ' · 留空保存将恢复默认名称'}`);
const nameDirty = computed(() => draftName.value !== (deviceName.value ?? ''));

function startRename(): void {
  draft.value = deviceName.value ?? '';
  editing.value = true;
  void nextTick(() => nameField.value?.focus());
}
async function submitName(name: string): Promise<void> {
  if (!session.isOwner || renaming.value || validateDeviceName(name)) return;
  renaming.value = true;
  try {
    const r = parseDeviceName(await session.request('network.name.set', { name }));
    const applied = r.name ?? name;
    if (applied) session.setDeviceName(applied);
    named.value = r.name ? r.named : name !== '';
    editing.value = false;
    toast.ok(name ? `设备已改名为「${applied}」` : applied ? `已恢复默认名称「${applied}」` : '已恢复默认名称');
  } catch (e) {
    const code = (e as { code?: string } | null)?.code;
    if (code === 'EINVAL') toast.show(`名称无效：最多 ${DEVICE_NAME_MAX_BYTES} 字节，且不能包含控制字符`, 'error');
    else if (code === 'ENOTFOUND') toast.show('此固件不支持修改设备名称', 'error');
    else toast.error(e, '修改设备名称失败');
  } finally {
    renaming.value = false;
  }
}
function saveName(): void {
  if (nameDirty.value) void submitName(draftName.value);
  else editing.value = false;
}
async function resetName(): Promise<void> {
  if (await dialog.confirm('设备名称和热点名称都会恢复为出厂默认值。', { title: '恢复默认名称？', confirmText: '恢复' })) await submitName('');
}

/* sys.demo has no implementation in the device build: the switch is hidden
 * there. It has no readback in sys.info; the switch mirrors the last ack. */
const demoAvailable = !__NYA_DEVICE__;
const demoOn = ref(false);
const demoBusy = ref(false);
async function setDemo(v: boolean): Promise<void> {
  demoBusy.value = true;
  try {
    const r = await session.request('sys.demo', { enabled: v });
    demoOn.value = r.enabled === undefined ? v : r.enabled === true;
    toast.ok(demoOn.value ? '演示轮播已开启' : '演示轮播已关闭');
  } catch (e) {
    toast.error(e, '切换演示轮播失败');
  } finally {
    demoBusy.value = false;
  }
}

function disconnect(): void {
  session.disconnect();
  void router.push({ name: 'connect' });
}
async function forget(): Promise<void> {
  const ok = await dialog.confirm('将清除本机保存的配对令牌，下次连接需重新配对。', { title: '忘记该设备？', danger: true, confirmText: '忘记' });
  if (!ok) return;
  session.forgetCurrent();
  void router.push({ name: 'connect' });
}
</script>

<template>
  <div class="grid-cards overview">
    <MdCard title="设备信息" class="span2">
      <Skeleton v-if="task.busy.value && !info" :lines="5" />
      <EmptyState v-else-if="!info" tone="error" compact icon="error" title="读取失败" hint="无法获取 sys.info" action-text="重试" @action="task.run()" />
      <template v-else>
        <dl class="kv">
          <div class="name-row">
            <dt>名称</dt>
            <dd class="name-val">
              <span class="name-text" :title="deviceName ?? undefined">{{ deviceName ?? '—' }}</span>
              <span v-if="named === false" class="tag" title="仍是出厂默认名称">默认</span>
              <MdButton v-if="session.isOwner && !editing" variant="icon" title="修改设备名称" aria-label="修改设备名称" @click="startRename"><UiIcon name="edit" :size="16" /></MdButton>
            </dd>
          </div>
          <div><dt>设备 ID</dt><dd class="mono">{{ info.device?.id ?? session.device?.id ?? '—' }}</dd></div>
          <div><dt>Core 版本</dt><dd class="mono">{{ info.device?.coreVersion ?? session.device?.coreVersion ?? '—' }}</dd></div>
          <div><dt>运行时长</dt><dd>{{ fmtUptime(info.uptime) }}</dd></div>
          <div><dt>我的角色</dt><dd>{{ session.role ? roleZh[session.role] ?? session.role : '—' }}</dd></div>
        </dl>
        <form v-if="editing" class="rename" @submit.prevent="saveName">
          <MdTextField ref="nameField" v-model="draft" label="设备名称" placeholder="留空 = 恢复默认名称" icon="edit" autocomplete="off" :disabled="renaming" :error="nameError" :hint="nameHint" />
          <p class="muted hint">这个名称同时也是设备配网热点的名称，以及路由器「已连接设备」列表里显示的主机名；路由器会在下一次 DHCP 续租后才更新显示。</p>
          <div class="row wrap rename-actions">
            <MdButton v-if="named !== false" variant="text" type="button" :disabled="renaming" @click="resetName">恢复默认</MdButton>
            <MdButton variant="text" type="button" :disabled="renaming" @click="editing = false">取消</MdButton>
            <MdButton type="submit" :disabled="renaming || !!nameError || !nameDirty">{{ renaming ? '保存中…' : '保存' }}</MdButton>
          </div>
        </form>
      </template>
    </MdCard>

    <MdCard :title="battery.external ? '供电' : '电量'">
      <div class="stat">
        <UiIcon :name="battery.charging || battery.external ? 'bolt' : 'battery'" :size="28" />
        <div class="stat-body">
          <div class="stat-value" :title="batteryValue">{{ batteryValue }}</div>
          <div class="stat-sub" :title="batterySub"><span class="stat-text">{{ batterySub }}</span></div>
        </div>
      </div>
      <div v-if="battery.level !== null" class="bar"><span :style="{ width: battery.level + '%' }" :class="{ low: battery.level < 20 }" /></div>
    </MdCard>

    <MdCard title="WiFi">
      <div class="stat">
        <UiIcon name="wifi" :size="28" />
        <div class="stat-body">
          <div class="stat-value" :title="wifi.ssid ?? undefined">{{ wifi.ssid ?? '未连接' }}</div>
          <div class="stat-sub" :title="wifiSub">
            <span class="stat-text">{{ wifiSub }}</span>
            <span v-if="wifi.rssi !== null" class="bars"><i v-for="n in 4" :key="n" :class="{ on: n <= rssiBars(wifi.rssi) }" /></span>
          </div>
        </div>
      </div>
    </MdCard>

    <MdCard v-if="demoAvailable && session.canControl" title="演示轮播">
      <div class="row between">
        <span class="muted" style="font-size: 13px">自动轮换表情与场景（手动控制后会停止）</span>
        <MdSwitch :model-value="demoOn" :disabled="demoBusy" @update:model-value="setDemo" />
      </div>
    </MdCard>

    <MdCard title="连接" class="span2">
      <div class="row wrap">
        <MdButton variant="outlined" @click="disconnect"><UiIcon name="link_off" :size="16" /> 断开连接</MdButton>
        <MdButton variant="text" class="danger" @click="forget"><UiIcon name="delete" :size="16" /> 忘记设备</MdButton>
      </div>
      <p class="muted hint">忘记设备会删除本机保存的配对令牌，不影响设备本身。</p>
    </MdCard>
  </div>
</template>

<style scoped>
.overview { grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); }
.span2 { grid-column: span 2; }
@media (max-width: 520px) { .span2 { grid-column: auto; } }
.stat { display: flex; align-items: center; gap: 14px; color: var(--md-primary); min-width: 0; }
.stat > .ui-icon { flex: none; }
/* The text column shrinks with the card; both lines ellipsize (full text in title). */
.stat-body { flex: 1; min-width: 0; }
.stat-value { font: 600 20px var(--font-title); color: var(--md-on-surface); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.stat-sub { font-size: 12.5px; color: var(--md-on-surface-variant); margin-top: 2px; display: flex; align-items: center; gap: 6px; min-width: 0; }
.stat-text { min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar { margin-top: 12px; height: 6px; border-radius: 999px; background: var(--md-surface-container-highest); overflow: hidden; }
.bar span { display: block; height: 100%; background: var(--md-success); border-radius: 999px; transition: width var(--dur); }
.bar span.low { background: var(--md-error); }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 12px; flex: none; }
.bars i { width: 3px; background: var(--md-outline-variant); border-radius: 1px; }
.bars i:nth-child(1) { height: 4px; }
.bars i:nth-child(2) { height: 7px; }
.bars i:nth-child(3) { height: 10px; }
.bars i:nth-child(4) { height: 12px; }
.bars i.on { background: var(--md-primary); }
.kv > .name-row { align-items: center; }
.name-val { display: flex; align-items: center; justify-content: flex-end; gap: 6px; min-width: 0; }
.name-text { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.name-val .tag { flex: none; }
.name-val .md-btn { flex: none; margin: -6px -6px -6px 0; }
.rename { display: flex; flex-direction: column; gap: 10px; margin-top: 14px; padding-top: 14px; border-top: 1px solid var(--md-outline-variant); }
.rename-actions { justify-content: flex-end; }
.hint { font-size: 12px; margin: 10px 0 0; }
.rename .hint { margin: 0; line-height: 1.6; }
.danger { color: var(--md-error) !important; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
.md-btn.icon :deep(.ui-icon) { margin-right: 0; vertical-align: middle; }
</style>
