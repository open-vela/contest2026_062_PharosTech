<script setup lang="ts">
/* Appearance block (skin / mode / palette) for every skin's account page.
 * Styled only with --md-* tokens so it reads native in all three skins;
 * `flat` drops the rounded chips for the instrument look. */
import { UiIcon, eventOrigin } from '@nyabula/ui';
import { useAppearance } from '../logic/useAppearance';

defineProps<{ flat?: boolean }>();
const ap = useAppearance();

/* Tiny per-skin glyph so the choice reads at a glance. */
const SKIN_ICON: Record<string, string> = { nebula: 'auto_awesome', daylight: 'light_mode', classic: 'dashboard', instrument: 'terminal' };
</script>

<template>
  <div class="ap" :class="{ flat }">
    <section>
      <h4 class="ap-h">界面风格</h4>
      <div class="ap-skins">
        <button
          v-for="s in ap.skins.value"
          :key="s.id"
          class="ap-skin"
          :class="[s.id, { on: ap.skin.value === s.id }]"
          @click="ap.setSkin(s.id, eventOrigin($event))"
        >
          <span class="ap-skin-ico"><UiIcon :name="SKIN_ICON[s.id] ?? 'palette'" :size="18" /></span>
          <span class="ap-skin-t"><b>{{ s.name }}</b><small>{{ s.desc }}</small></span>
          <UiIcon v-if="ap.skin.value === s.id" name="check_circle" :size="18" class="ap-check" />
        </button>
      </div>
    </section>

    <section>
      <h4 class="ap-h">外观模式</h4>
      <div class="ap-modes">
        <button
          v-for="c in ap.modeChoices"
          :key="c.id"
          class="ap-mode"
          :class="{ on: ap.mode.value === c.id }"
          @click="ap.setMode(c.id, eventOrigin($event))"
        >
          <UiIcon :name="c.icon" :size="18" /><span>{{ c.label }}</span>
        </button>
      </div>
    </section>

    <section>
      <h4 class="ap-h">色盘</h4>
      <div class="ap-palettes">
        <button
          v-for="p in ap.palettes.value"
          :key="p.id"
          class="ap-pal"
          :class="{ on: ap.paletteId.value === p.id }"
          :title="p.name"
          @click="ap.setPalette(p.id, eventOrigin($event))"
        >
          <span class="ap-dot" :style="{ background: `linear-gradient(135deg, ${p[ap.theme.mode].primary}, ${p[ap.theme.mode].primaryDeep})` }">
            <UiIcon v-if="ap.paletteId.value === p.id" name="check" :size="15" />
          </span>
          <span class="ap-pal-n">{{ p.name }}</span>
        </button>
      </div>
    </section>
  </div>
</template>

<style scoped>
.ap { display: flex; flex-direction: column; gap: 20px; }
.ap-h { margin: 0 0 10px; font: 600 12px var(--font-body); letter-spacing: 0.08em; color: var(--md-on-surface-variant); }
.ap-skins { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px; }
.ap-skin {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  gap: 10px;
  padding: 12px;
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-m);
  background: transparent;
  color: var(--md-on-surface);
  text-align: left;
  cursor: pointer;
  transition: border-color var(--dur-fast), background var(--dur-fast), transform var(--dur-fast) var(--ease-spring);
}
.ap-skin:hover { background: var(--md-surface-container-high); transform: translateY(-2px); }
.ap-skin.on { border-color: var(--md-primary); background: rgba(var(--md-primary-rgb), 0.08); }
.ap-skin-ico { width: 34px; height: 34px; border-radius: 10px; display: grid; place-items: center; background: var(--md-surface-container-highest); color: var(--md-primary); }
.ap-skin.nebula .ap-skin-ico { background: radial-gradient(circle at 30% 30%, rgba(var(--md-primary-rgb), 0.5), var(--md-surface-container-highest) 70%); }
.ap-skin.instrument .ap-skin-ico { border-radius: 4px; }
.ap-skin-t { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.ap-skin-t b { font-weight: 600; font-size: 14px; }
.ap-skin-t small { font-size: 11.5px; color: var(--md-on-surface-variant); line-height: 1.35; }
.ap-check { position: absolute; top: 10px; right: 10px; color: var(--md-primary); }
.ap-modes { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
.ap-mode {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 10px 6px;
  border: 1px solid var(--md-outline-variant);
  border-radius: var(--radius-m);
  background: transparent;
  color: var(--md-on-surface-variant);
  font: 600 13px var(--font-body);
  cursor: pointer;
  transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast);
}
.ap-mode:hover { background: var(--md-surface-container-high); }
.ap-mode.on { background: var(--md-secondary-container); color: var(--md-on-secondary-container); border-color: transparent; }
.ap-palettes { display: flex; flex-wrap: wrap; gap: 14px; }
.ap-pal { display: flex; flex-direction: column; align-items: center; gap: 6px; border: none; background: transparent; cursor: pointer; padding: 2px; }
.ap-dot {
  width: 38px;
  height: 38px;
  border-radius: 50%;
  display: grid;
  place-items: center;
  color: #fff;
  box-shadow: 0 4px 12px -4px rgba(0, 0, 0, 0.45);
  transition: transform var(--dur-fast) var(--ease-spring), box-shadow var(--dur-fast);
}
.ap-pal:hover .ap-dot { transform: scale(1.08); }
.ap-pal.on .ap-dot { box-shadow: 0 0 0 3px var(--md-surface), 0 0 0 5px var(--md-primary); }
.ap-pal-n { font-size: 12px; color: var(--md-on-surface-variant); }
.ap-pal.on .ap-pal-n { color: var(--md-on-surface); font-weight: 600; }
.flat .ap-skin, .flat .ap-mode { border-radius: 4px; }
.flat .ap-skin-ico { border-radius: 3px; }
.flat .ap-dot { border-radius: 3px; }
@media (max-width: 520px) {
  .ap-skins { grid-template-columns: 1fr; }
  .ap-skin { flex-direction: row; align-items: center; }
}
</style>
