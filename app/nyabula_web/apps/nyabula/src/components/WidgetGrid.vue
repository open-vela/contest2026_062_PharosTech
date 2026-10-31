<script setup lang="ts">
/* Renders host-registered widgets plus plugin "widget" surfaces (nyaui v1.1
 * proposal: plugin.list[].ui includes "widget"). Empty = renders nothing. */
import { computed } from 'vue';
import { hostRegistry } from '../host/registry';
import { usePluginsStore } from '../stores/plugins';
import type { FormFactor } from '../composables/useFormFactor';
import PluginWidget from './PluginWidget.vue';

const props = defineProps<{ formFactor: FormFactor }>();
const plugins = usePluginsStore();
const hostWidgets = computed(() => hostRegistry.widgets(props.formFactor));
const pluginWidgets = computed(() => plugins.list.filter((p) => p.ui?.includes('widget') || p.surfaces?.includes('widget')));
const any = computed(() => hostWidgets.value.length + pluginWidgets.value.length > 0);
</script>

<template>
  <div v-if="any" class="widgets">
    <p class="section-title">小组件</p>
    <div class="widget-grid">
      <component
        :is="w.component"
        v-for="w in hostWidgets"
        :key="w.id"
        class="widget"
        :style="{ gridColumn: `span ${w.span ?? 1}` }"
      />
      <PluginWidget v-for="p in pluginWidgets" :key="p.id" :plugin="p" class="widget" />
    </div>
  </div>
</template>

<style scoped>
.widget-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 12px; }
</style>
