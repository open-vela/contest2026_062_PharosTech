<script lang="ts">
type IconAsset = { version: string; mime: string; data: string };
const cache = new WeakMap<object, Map<string, Promise<IconAsset>>>();
let nextFont = 0;
</script>

<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { UiIcon } from '@nyabula/ui';
import type { PluginInfo } from '../stores/plugins';
import { useSessionStore } from '../stores/session';

const props = withDefaults(defineProps<{ plugin?: PluginInfo | null; size?: number }>(), { size: 24 });
const session = useSessionStore();
const image = ref('');
const font = ref('');
const glyph = ref('');
const name = computed(() => typeof props.plugin?.icon === 'string' ? props.plugin.icon : 'extension');
const box = computed(() => ({ width: `${props.size}px`, height: `${props.size}px` }));

watch(() => [session.client, props.plugin?.id, props.plugin?.version, JSON.stringify(props.plugin?.icon)] as const,
  async (_, __, cleanup) => {
    image.value = ''; font.value = ''; glyph.value = '';
    const plugin = props.plugin;
    const spec = plugin?.icon;
    const client = session.client;
    if (!client || !plugin || !spec || typeof spec === 'string') return;
    let cancelled = false;
    let face: FontFace | undefined;
    cleanup(() => { cancelled = true; if (face) document.fonts.delete(face); });
    let entries = cache.get(client);
    if (!entries) { entries = new Map(); cache.set(client, entries); }
    const key = `${plugin.id}:${plugin.version}:${spec.path}`;
    let pending = entries.get(key);
    if (!pending) {
      pending = session.request('plugin.icon', { pluginId: plugin.id }).then(value => value as unknown as IconAsset);
      entries.set(key, pending);
      pending.catch(() => entries?.delete(key));
    }
    try {
      const asset = await pending;
      if (cancelled || asset.version !== plugin.version || asset.data.length > 21848 || !/^[A-Za-z0-9+/]*={0,2}$/.test(asset.data)) return;
      if (spec.type === 'image' && ['image/png', 'image/jpeg', 'image/webp'].includes(asset.mime)) {
        image.value = `data:${asset.mime};base64,${asset.data}`;
      } else if (spec.type === 'iconfont' && asset.mime === 'font/woff2' && /^[0-9a-f]{1,6}$/i.test(spec.codepoint ?? '')) {
        const codepoint = parseInt(spec.codepoint!, 16);
        if (codepoint < 32 || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) return;
        const family = `ny-plugin-icon-${++nextFont}`;
        face = new FontFace(family, `url("data:font/woff2;base64,${asset.data}")`);
        await face.load();
        if (cancelled) return;
        document.fonts.add(face);
        font.value = family;
        glyph.value = String.fromCodePoint(codepoint);
      }
    } catch {
      // Keep the built-in fallback when an asset is absent or cannot decode.
    }
  }, { immediate: true });
</script>

<template>
  <img v-if="image" class="plugin-icon image" :style="box" :src="image" alt="" @error="image = ''" />
  <span v-else-if="font" class="plugin-icon glyph" :style="{ ...box, fontFamily: font, fontSize: `${size}px` }" aria-hidden="true">{{ glyph }}</span>
  <UiIcon v-else :name="name" :size="size" />
</template>

<style scoped>
.plugin-icon { display: inline-block; flex: none; vertical-align: middle; }
.image { object-fit: contain; border-radius: 20%; }
.glyph { text-align: center; line-height: 1; font-weight: normal; font-style: normal; }
</style>
