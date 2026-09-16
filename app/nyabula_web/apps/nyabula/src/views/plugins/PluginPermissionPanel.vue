<script setup lang="ts">
/* Permission switch list for one plugin (context panel / tab / sheet). */
import { toRef, type Ref } from 'vue';
import { EmptyState } from '@nyabula/ui';
import PermissionRow from './PermissionRow.vue';
import { usePluginPermissions } from './plugins.logic';

const props = defineProps<{ id: string; compact?: boolean }>();
const p = usePluginPermissions(toRef(props, 'id') as Ref<string>);
</script>

<template>
  <div class="perm-panel">
    <p v-if="!p.canEdit.value" class="muted note">
      {{ p.session.connected ? '仅设备主人可以更改权限' : '设备未连接，无法更改权限' }}
    </p>
    <EmptyState v-if="p.perms.value.length === 0" compact icon="shield" title="无需权限" hint="此插件未申请任何权限" />
    <div v-else class="stack">
      <PermissionRow
        v-for="perm in p.perms.value"
        :key="perm.name"
        :name="perm.name"
        :granted="perm.granted"
        :disabled="!p.canEdit.value"
        :busy="!!p.pending.value[perm.name]"
        :compact="compact"
        @toggle="p.toggle(perm.name, $event)"
      />
    </div>
  </div>
</template>

<style scoped>
.perm-panel { display: flex; flex-direction: column; gap: 10px; }
.note { font-size: 12.5px; margin: 0; }
</style>
