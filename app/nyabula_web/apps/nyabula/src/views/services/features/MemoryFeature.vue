<script setup lang="ts">
/* Memory records live in Core. Only selection and unsaved input stay local. */
import { computed, reactive, ref, watch } from 'vue';
import { NkActionBar, NkChipSelect, MdButton, MdTextField, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeScene } from '../../../composables/useEyeScene';
import { memoryScene } from '../../../composables/eyeScenePayload';
import { useProductRecords } from '../../../composables/useProductRecords';
import type { ProductRecord } from '../../../stores/product';
import type { FormFactor } from '../../../composables/useFormFactor';

const props = defineProps<{ type: string; ff: FormFactor }>();
interface Card extends ProductRecord { id:string; text:string; at:number; tag:string }
const core = useProductRecords<Card>('memory');
const TAGS = [
  { id:'daily', label:'日常' }, { id:'family', label:'家人' }, { id:'pet', label:'猫咪' },
  { id:'todo', label:'待办' }, { id:'fun', label:'趣事' },
];
const TAG_LABEL: Record<string,string> = Object.fromEntries(TAGS.map(t=>[t.id,t.label]));
const state = reactive({selected:''});
const query = ref('');
const newText = ref('');
const newTag = ref<string|string[]>('daily');
const filterTag = ref<string|string[]>('');
const cards = computed(() => core.items.value.map(c=>({...c,tag:typeof c.tag==='string'?c.tag:'daily',at:typeof c.at==='number'?c.at:0})));
watch(cards, list => { if(!list.some(c=>c.id===state.selected)) state.selected=list[0]?.id??''; });
const filtered = computed(() => {
  const q=query.value.trim().toLowerCase();
  const tag=typeof filterTag.value==='string'?filterTag.value:'';
  return [...cards.value].filter(c=>(!tag||c.tag===tag)&&(!q||c.text.toLowerCase().includes(q)||TAG_LABEL[c.tag]?.includes(q))).sort((a,b)=>b.at-a.at);
});
const current = computed<Card|null>(()=>cards.value.find(c=>c.id===state.selected)??filtered.value[0]??null);
/* Eye link: the selected memory. */
const scene = useEyeScene(props.type, () => { const card=current.value; return card ? memoryScene({text:card.text,tag:TAG_LABEL[card.tag]??card.tag}) : null; }, {hideWhenEmpty:true});
const active = computed(()=>scene.shown.value||scene.held.value);
const subtitle = computed(()=>core.available.value ? `${cards.value.length} 条设备记忆` : '未连接支持此功能的 Core');
function fmt(ts:number) {
  if(ts<1577836800000) return '设备时间未校准';
  const d=new Date(ts), diff=Date.now()-ts;
  if(diff>=0&&diff<3600e3) return `${Math.max(1,Math.floor(diff/60e3))} 分钟前`;
  if(diff>=0&&diff<86400e3) return `${Math.floor(diff/3600e3)} 小时前`;
  return `${d.getMonth()+1}月${d.getDate()}日`;
}
async function add() {
  const text=newText.value.trim();
  if(!text) return;
  if(await core.mutate('create',{record:{text,tag:typeof newTag.value==='string'?newTag.value:'daily'}})) {
    state.selected=core.items.value[core.items.value.length-1]?.id??'';
    newText.value='';
  }
}
async function remove(card:Card) { await core.mutate('delete',{id:card.id}); }
const recall = () => scene.show();
const hide = () => scene.hide();
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="memory" title="记忆" :subtitle="subtitle" :tone="active ? 'ok' : 'default'" />
    <p v-if="core.error.value" role="alert" class="muted">{{ core.error.value }}</p>
    <div class="grid">
      <section class="card">
        <MdTextField v-model="query" label="搜索" placeholder="搜索记忆内容或标签" icon="search" />
        <NkChipSelect v-model="filterTag" :options="[{ id: '', label: '全部' }, ...TAGS]" label="按标签" />
        <div class="cards">
          <button v-for="c in filtered" :key="c.id" type="button" class="mcard" :class="{ sel: c.id === current?.id }" @click="state.selected = c.id">
            <span class="mcard-text">{{ c.text }}</span>
            <span class="mcard-meta">
              <span class="tag info">{{ TAG_LABEL[c.tag] ?? c.tag }}</span>
              <span class="muted">{{ fmt(c.at) }}</span>
            </span>
          </button>
          <p v-if="!filtered.length" class="muted">没有匹配的记忆。</p>
        </div>
      </section>
      <section class="card">
        <h3 class="section-title">回顾</h3>
        <div v-if="current" class="recall">
          <p class="recall-text">{{ current.text }}</p>
          <div class="row between">
            <span class="muted">{{ TAG_LABEL[current.tag] ?? current.tag }} · {{ fmt(current.at) }}</span>
            <MdButton variant="text" :disabled="core.busy.value || !core.available.value" @click="remove(current)"><UiIcon name="delete" :size="18" />删除</MdButton>
          </div>
        </div>
        <p v-else class="muted">选择一条记忆。</p>
        <NkActionBar primary-text="回顾一条到眼睛" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" :disabled="!scene.canShow.value" @primary="recall" @secondary="hide" />
        <h3 class="section-title">新建记忆</h3>
        <MdTextField v-model="newText" label="内容" placeholder="想让它记住什么" @keydown.enter="add" />
        <NkChipSelect v-model="newTag" :options="TAGS" label="标签" />
        <MdButton variant="tonal" :disabled="!newText.trim() || core.busy.value || !core.available.value" @click="add"><UiIcon name="add" :size="20" />{{ core.busy.value ? '保存中…' : '记住' }}</MdButton>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid, .feature.tablet .grid { grid-template-columns: 1.2fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.cards { display: grid; grid-template-columns: 1fr; gap: 10px; }
.feature.desktop .cards { grid-template-columns: 1fr 1fr; }
.mcard {
  display: flex; flex-direction: column; gap: 8px; min-height: 64px; padding: 12px 14px; border: 0; cursor: pointer; text-align: left;
  border-radius: var(--radius-m); background: var(--md-surface-container-high); color: var(--md-on-surface);
  transition: background var(--dur-short) var(--ease-standard);
}
.mcard.sel { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.mcard-text { line-height: 1.5; }
.mcard-meta { display: flex; align-items: center; gap: 8px; font-size: 12px; }
.recall { padding: 14px; border-radius: var(--radius-m); background: var(--md-surface-container-high); }
.recall-text { margin: 0 0 8px; font-size: 16px; line-height: 1.6; }
</style>
