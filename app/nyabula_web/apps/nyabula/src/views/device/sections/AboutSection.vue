<script setup lang="ts">
/* About: versions and the contract documents this client speaks. */
import { MdCard, UiIcon } from '@nyabula/ui';
import { useSessionStore } from '../../../stores/session';

const session = useSessionStore();
const WEB_VERSION = '0.2.0';
const docs = [
  { icon: 'link', name: 'NyaLink', file: 'Shared/protocol/nyalink.md', desc: '设备 ⇄ 客户端 WebSocket 协议（v1）' },
  { icon: 'cloud', name: 'Cloud', file: 'Shared/protocol/cloud.md', desc: '云中继与账号认领' },
  { icon: 'widgets', name: 'NyaUI', file: 'Shared/nyaui/nyaui.md', desc: '插件声明式 UI 组件树' },
  { icon: 'visibility', name: 'Eye Params', file: 'packages/eye-params/eye-params.json', desc: '眼睛表情 / 场景状态机参数' },
];
</script>

<template>
  <div class="stack">
    <MdCard title="版本">
      <dl class="kv">
        <div><dt>设备名称</dt><dd>{{ session.device?.name ?? '—' }}</dd></div>
        <div><dt>Core 版本</dt><dd class="mono">{{ session.device?.coreVersion ?? '—' }}</dd></div>
        <div><dt>Web 客户端</dt><dd class="mono">{{ WEB_VERSION }}</dd></div>
        <div><dt>NyaLink 协议</dt><dd class="mono">v1</dd></div>
        <div><dt>连接方式</dt><dd>{{ session.transport === 'cloud' ? '云中继' : session.transport === 'lan' ? '局域网' : '—' }}</dd></div>
      </dl>
    </MdCard>
    <MdCard title="契约文件">
      <div class="docs">
        <div v-for="d in docs" :key="d.file" class="doc">
          <span class="doc-icon"><UiIcon :name="d.icon" :size="20" /></span>
          <span class="doc-body">
            <span class="doc-name">{{ d.name }}</span>
            <span class="doc-desc">{{ d.desc }}</span>
            <span class="doc-file mono">{{ d.file }}</span>
          </span>
        </div>
      </div>
    </MdCard>
    <p class="muted foot">Nyabula · Pharos Tech · Apache-2.0</p>
  </div>
</template>

<style scoped>
.docs { display: flex; flex-direction: column; gap: 4px; }
.doc { display: flex; gap: 12px; align-items: flex-start; padding: 8px 0; }
.doc + .doc { border-top: 1px solid var(--md-outline-variant); }
.doc-icon {
  width: 36px;
  height: 36px;
  border-radius: var(--radius-m);
  display: grid;
  place-items: center;
  background: rgba(var(--md-primary-rgb), 0.12);
  color: var(--md-primary);
  flex: none;
}
.doc-body { display: flex; flex-direction: column; min-width: 0; }
.doc-name { font: 600 14px var(--font-body); color: var(--md-on-surface); }
.doc-desc { font-size: 12.5px; color: var(--md-on-surface-variant); }
.doc-file { font-size: 11.5px; color: var(--md-outline); word-break: break-all; }
.foot { font-size: 12px; text-align: center; margin: 8px 0 0; }
</style>
