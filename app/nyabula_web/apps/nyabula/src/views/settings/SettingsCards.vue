<script setup lang="ts">
/* Card-style settings blocks shared by desktop / tablet variants. `only`
 * picks a subset so variants can arrange them in columns. */
import { MdButton, MdCard, MdSwitch, UiIcon, eventOrigin } from '@nyabula/ui';
import type { useSettingsPage } from './settings.logic';

defineProps<{ page: ReturnType<typeof useSettingsPage>; only?: ('theme' | 'palette' | 'layout' | 'dev' | 'data')[] }>();
</script>

<template>
  <MdCard v-if="!only || only.includes('theme')" title="外观模式">
    <div class="choices">
      <button
        v-for="c in page.modeChoices"
        :key="c.id"
        class="choice"
        :class="{ on: page.mode.value === c.id }"
        @click="page.setMode(c.id, eventOrigin($event))"
      >
        <UiIcon :name="c.icon" :size="20" />
        <span>{{ c.label }}</span>
      </button>
    </div>
    <p class="muted hint">当前：{{ page.theme.mode === 'dark' ? '深色' : '浅色' }}{{ page.mode.value === 'system' ? '（系统）' : '' }}</p>
  </MdCard>

  <MdCard v-if="!only || only.includes('palette')" title="色盘">
    <div class="palettes">
      <button
        v-for="p in page.palettes.value"
        :key="p.id"
        class="swatch"
        :class="{ on: page.paletteId.value === p.id }"
        :title="p.name"
        @click="page.setPalette(p.id, eventOrigin($event))"
      >
        <span class="dot" :style="{ background: `linear-gradient(135deg, ${p[page.theme.mode].primary}, ${p[page.theme.mode].primaryDeep})` }">
          <UiIcon v-if="page.paletteId.value === p.id" name="check" :size="16" />
        </span>
        <span class="swatch-name">{{ p.name }}</span>
      </button>
    </div>
  </MdCard>

  <MdCard v-if="!only || only.includes('layout')" title="布局">
    <div class="choices four">
      <button v-for="c in page.layoutChoices" :key="c.id" class="choice" :class="{ on: page.layout.value === c.id }" @click="page.setLayout(c.id)">
        <UiIcon :name="c.icon" :size="20" />
        <span>{{ c.label }}</span>
      </button>
    </div>
    <p class="muted hint">自动检测为：{{ { desktop: '桌面', tablet: '平板', phone: '手机' }[page.layoutAuto.value] }}。覆盖后所有页面按所选形态渲染。</p>
  </MdCard>

  <MdCard v-if="!only || only.includes('dev')" title="开发者">
    <div class="row between">
      <span>
        <span class="lbl">开发模式</span>
        <span class="muted sub">插件页显示原始 NyaUI 组件树</span>
      </span>
      <MdSwitch :model-value="page.dev.value" @update:model-value="page.setDev" />
    </div>
  </MdCard>

  <MdCard v-if="!only || only.includes('data')" title="本机数据">
    <p class="muted hint" style="margin-top: 0">已知设备、配对令牌与账号会话保存在本浏览器（{{ page.localCount.value }} 项）。</p>
    <MdButton variant="outlined" class="danger" @click="page.clearLocal()"><UiIcon name="delete" :size="16" /> 清除本机数据</MdButton>
  </MdCard>
</template>

<style scoped>
.choices { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
.choices.four { grid-template-columns: repeat(4, 1fr); }
.choice {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 12px 6px;
  border-radius: var(--radius-m);
  border: 1px solid var(--md-outline-variant);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 12.5px var(--font-body);
  cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast);
}
.choice:hover { background: var(--md-surface-container-high); }
.choice.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.hint { font-size: 12.5px; margin: 12px 0 0; }
.palettes { display: flex; flex-wrap: wrap; gap: 14px; }
.swatch { display: flex; flex-direction: column; align-items: center; gap: 6px; border: none; background: transparent; cursor: pointer; padding: 4px; }
.dot {
  width: 40px;
  height: 40px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  color: #fff;
  overflow: hidden;
  isolation: isolate;
  box-shadow: 0 4px 12px -4px rgba(0, 0, 0, 0.45);
  transition: transform var(--dur-fast) var(--ease-spring), box-shadow var(--dur-fast);
}
.dot :deep(.ui-icon) { filter: drop-shadow(0 1px 1px rgba(0, 0, 0, 0.45)); }
.swatch:hover .dot { transform: scale(1.08); }
.swatch.on .dot { box-shadow: 0 0 0 3px var(--md-surface-container), 0 0 0 5px var(--md-primary); }
.swatch-name { font-size: 12px; color: var(--md-on-surface-variant); }
.swatch.on .swatch-name { color: var(--md-on-surface); font-weight: 600; }
.lbl { display: block; font-size: 14px; color: var(--md-on-surface); }
.sub { display: block; font-size: 12.5px; margin-top: 2px; }
.danger { color: var(--md-error) !important; border-color: var(--md-error) !important; }
.md-btn :deep(.ui-icon) { vertical-align: -3px; margin-right: 4px; }
</style>
