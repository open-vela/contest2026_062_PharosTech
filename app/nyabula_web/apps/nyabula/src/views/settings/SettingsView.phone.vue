<script setup lang="ts">
/* Phone: grouped list; choice pickers open in bottom sheets. */
import { ref } from 'vue';
import { BottomSheet, MdSwitch, UiIcon, eventOrigin } from '@nyabula/ui';
import { useSettingsPage } from './settings.logic';

const page = useSettingsPage();
const sheet = ref<'mode' | 'palette' | 'layout' | null>(null);
const layoutZh: Record<string, string> = { auto: '自动', desktop: '桌面', tablet: '平板', phone: '手机' };
</script>

<template>
  <div class="settings-phone">
    <p class="section-title">外观</p>
    <div class="group">
      <button class="list-tile" @click="sheet = 'mode'">
        <span class="tile-icon"><UiIcon :name="page.theme.mode === 'dark' ? 'dark_mode' : 'light_mode'" :size="22" /></span>
        <span class="tile-body"><span class="tile-title">模式</span><span class="tile-sub">{{ page.modeChoices.find((c) => c.id === page.mode.value)?.label }}</span></span>
        <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
      </button>
      <button class="list-tile" @click="sheet = 'palette'">
        <span class="tile-icon"><UiIcon name="palette" :size="22" /></span>
        <span class="tile-body"><span class="tile-title">色盘</span><span class="tile-sub">{{ page.theme.palette.name }}</span></span>
        <span class="tile-trail"><span class="mini-dot" :style="{ background: page.theme.palette[page.theme.mode].primary }" /><UiIcon name="chevron_right" :size="20" /></span>
      </button>
      <button class="list-tile" @click="sheet = 'layout'">
        <span class="tile-icon"><UiIcon name="tune" :size="22" /></span>
        <span class="tile-body"><span class="tile-title">布局</span><span class="tile-sub">{{ layoutZh[page.layout.value] }}<template v-if="page.layout.value === 'auto'">（检测为{{ layoutZh[page.layoutAuto.value] }}）</template></span></span>
        <span class="tile-trail"><UiIcon name="chevron_right" :size="20" /></span>
      </button>
    </div>

    <p class="section-title">开发者</p>
    <div class="group">
      <div class="list-tile static">
        <span class="tile-icon"><UiIcon name="terminal" :size="22" /></span>
        <span class="tile-body"><span class="tile-title">开发模式</span><span class="tile-sub">插件页显示原始 NyaUI 组件树</span></span>
        <span class="tile-trail"><MdSwitch :model-value="page.dev.value" @update:model-value="page.setDev" /></span>
      </div>
    </div>

    <p class="section-title">数据</p>
    <div class="group">
      <button class="list-tile" @click="page.clearLocal()">
        <span class="tile-icon danger"><UiIcon name="delete" :size="22" /></span>
        <span class="tile-body"><span class="tile-title danger">清除本机数据</span><span class="tile-sub">已知设备、配对令牌、账号会话（{{ page.localCount.value }} 项）</span></span>
      </button>
    </div>

    <BottomSheet :open="sheet === 'mode'" title="外观模式" @close="sheet = null">
      <div class="sheet-list">
        <button v-for="c in page.modeChoices" :key="c.id" class="sheet-item" :class="{ on: page.mode.value === c.id }" @click="page.setMode(c.id, eventOrigin($event)); sheet = null">
          <UiIcon :name="c.icon" :size="20" /><span>{{ c.label }}</span><UiIcon v-if="page.mode.value === c.id" name="check" :size="18" class="trail" />
        </button>
      </div>
    </BottomSheet>
    <BottomSheet :open="sheet === 'palette'" title="色盘" @close="sheet = null">
      <div class="sheet-list">
        <button v-for="p in page.palettes.value" :key="p.id" class="sheet-item" :class="{ on: page.paletteId.value === p.id }" @click="page.setPalette(p.id, eventOrigin($event)); sheet = null">
          <span class="mini-dot big" :style="{ background: `linear-gradient(135deg, ${p[page.theme.mode].primary}, ${p[page.theme.mode].primaryDeep})` }" /><span>{{ p.name }}</span><UiIcon v-if="page.paletteId.value === p.id" name="check" :size="18" class="trail" />
        </button>
      </div>
    </BottomSheet>
    <BottomSheet :open="sheet === 'layout'" title="布局覆盖" @close="sheet = null">
      <div class="sheet-list">
        <button v-for="c in page.layoutChoices" :key="c.id" class="sheet-item" :class="{ on: page.layout.value === c.id }" @click="page.setLayout(c.id); sheet = null">
          <UiIcon :name="c.icon" :size="20" /><span>{{ c.label }}</span><UiIcon v-if="page.layout.value === c.id" name="check" :size="18" class="trail" />
        </button>
      </div>
    </BottomSheet>
  </div>
</template>

<style scoped>
.settings-phone { padding: 8px 14px calc(28px + var(--shell-bottom) + var(--safe-b)); }
.group { display: flex; flex-direction: column; gap: 4px; }
.list-tile { width: 100%; text-align: left; border: none; font: inherit; }
.list-tile.static { cursor: default; }
.mini-dot { width: 14px; height: 14px; border-radius: 50%; display: inline-block; }
.mini-dot.big { width: 22px; height: 22px; }
.danger { color: var(--md-error) !important; }
.sheet-list { display: flex; flex-direction: column; gap: 2px; }
.sheet-item {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 10px;
  border: none;
  border-radius: var(--radius-m);
  background: transparent;
  color: var(--md-on-surface);
  font: 500 14.5px var(--font-body);
  text-align: left;
  cursor: pointer;
}
.sheet-item.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); font-weight: 600; }
.sheet-item .trail { margin-left: auto; }
</style>
