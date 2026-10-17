<script setup lang="ts">
/* Overview: sys.info readout, demo carousel toggle (family+), disconnect /
 * forget device. */
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { EmptyState, MdButton, MdCard, MdSwitch, Skeleton, UiIcon, useDialogStore, useToastStore } from '@nyabula/ui';
import { batteryOf, fmtUptime, rssiBars, useSysInfo, wifiOf } from './sysinfo';

const router = useRouter();
const dialog = useDialogStore();
const toast = useToastStore();
const { session, task, info } = useSysInfo();

const battery = computed(() => batteryOf(info.value));
const wifi = computed(() => wifiOf(info.value));
const roleZh: Record<string, string> = { owner: '主人 (owner)', family: '家人 (family)', guest: '访客 (guest)' };

/* sys.demo has no readback in sys.info; the switch mirrors the last ack. */
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
      <dl v-else class="kv">
        <div><dt>名称</dt><dd>{{ info.device?.name ?? session.device?.name ?? '—' }}</dd></div>
        <div><dt>设备 ID</dt><dd class="mono">{{ info.device?.id ?? session.device?.id ?? '—' }}</dd></div>
        <div><dt>Core 版本</dt><dd class="mono">{{ info.device?.coreVersion ?? session.device?.coreVersion ?? '—' }}</dd></div>
        <div><dt>运行时长</dt><dd>{{ fmtUptime(info.uptime) }}</dd></div>
        <div><dt>我的角色</dt><dd>{{ session.role ? roleZh[session.role] ?? session.role : '—' }}</dd></div>
      </dl>
    </MdCard>

    <MdCard title="电量">
      <div class="stat">
        <UiIcon :name="battery.charging ? 'bolt' : 'battery'" :size="28" />
        <div>
          <div class="stat-value">{{ battery.level !== null ? battery.level + '%' : '—' }}</div>
          <div class="stat-sub">{{ battery.charging ? '充电中' : battery.level !== null ? '使用电池' : '无数据' }}</div>
        </div>
      </div>
      <div v-if="battery.level !== null" class="bar"><span :style="{ width: battery.level + '%' }" :class="{ low: battery.level < 20 }" /></div>
    </MdCard>

    <MdCard title="WiFi">
      <div class="stat">
        <UiIcon name="wifi" :size="28" />
        <div>
          <div class="stat-value">{{ wifi.ssid ?? '未连接' }}</div>
          <div class="stat-sub">
            <template v-if="wifi.rssi !== null">{{ wifi.rssi }} dBm · <span class="bars"><i v-for="n in 4" :key="n" :class="{ on: n <= rssiBars(wifi.rssi) }" /></span></template>
            <template v-else>无信号数据</template>
          </div>
        </div>
      </div>
    </MdCard>

    <MdCard v-if="session.canControl" title="演示轮播">
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
.stat { display: flex; align-items: center; gap: 14px; color: var(--md-primary); }
.stat-value { font: 600 20px var(--font-title); color: var(--md-on-surface); }
.stat-sub { font-size: 12.5px; color: var(--md-on-surface-variant); margin-top: 2px; display: flex; align-items: center; gap: 4px; }
.bar { margin-top: 12px; height: 6px; border-radius: 999px; background: var(--md-surface-container-highest); overflow: hidden; }
.bar span { display: block; height: 100%; background: var(--md-success); border-radius: 999px; transition: width var(--dur); }
.bar span.low { background: var(--md-error); }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 12px; }
.bars i { width: 3px; background: var(--md-outline-variant); border-radius: 1px; }
.bars i:nth-child(1) { height: 4px; }
.bars i:nth-child(2) { height: 7px; }
.bars i:nth-child(3) { height: 10px; }
.bars i:nth-child(4) { height: 12px; }
.bars i.on { background: var(--md-primary); }
.hint { font-size: 12px; margin: 10px 0 0; }
.danger { color: var(--md-error) !important; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
