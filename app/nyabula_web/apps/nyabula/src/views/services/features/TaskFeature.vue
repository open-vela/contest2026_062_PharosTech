<script setup lang="ts">
/* Manual task records live in Core. Status editing is an explicit draft. */
import { computed, reactive, ref, watch } from 'vue';
import { NkActionBar, NkListSection, NkProgressRing, NkSegmentRow, NkSliderRow, MdButton, MdTextField, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader as NkHeader } from '../featureHeader';
import { useEyeStore } from '../../../stores/eye';
import { useProductRecords } from '../../../composables/useProductRecords';
import type { ProductRecord } from '../../../stores/product';
import type { FormFactor } from '../../../composables/useFormFactor';

const props=defineProps<{type:string;ff:FormFactor}>();
const eye=useEyeStore();
type TaskState='queued'|'running'|'confirm'|'done'|'failed'|'cancelled';
interface Task extends ProductRecord {id:string;title:string;state:TaskState;progress:number;done:boolean}
const core=useProductRecords<Task>('task');
const STATES=[
  {id:'queued',label:'排队'},{id:'running',label:'进行中'},{id:'confirm',label:'待确认'},
  {id:'done',label:'完成'},{id:'failed',label:'失败'},{id:'cancelled',label:'取消'},
];
const tasks=computed(()=>core.items.value.map(t=>({...t,progress:typeof t.progress==='number'?t.progress:t.state==='done'?100:0,done:t.state==='done'})));
const state=reactive({selected:'',get tasks(){return tasks.value;}});
watch(tasks,list=>{if(!list.some(t=>t.id===state.selected))state.selected=list[0]?.id??'';});
const newTitle=ref('');
const total=computed(()=>tasks.value.length);
const doneCount=computed(()=>tasks.value.filter(t=>t.done).length);
const ratio=computed(()=>total.value?doneCount.value/total.value:0);
const current=computed<Task|null>(()=>tasks.value.find(t=>t.id===state.selected)??tasks.value[0]??null);
const active=computed(()=>eye.activeScene===props.type);
const subtitle=computed(()=>core.available.value?`完成 ${doneCount.value} / ${total.value}`:'未连接支持此功能的 Core');
const draftState=ref<TaskState>('queued');
const draftProgress=ref(0);
const draftRevision=ref(0);
const draftId=ref('');
const dirty=ref(false);
watch(current,t=>{
  if(t&&(!dirty.value||draftId.value!==t.id)){
    draftId.value=t.id;draftState.value=t.state;draftProgress.value=t.progress;
    draftRevision.value=core.revision.value;dirty.value=false;
  }
},{immediate:true});
const curState=computed({
  get:()=>draftState.value,
  set:(v:string)=>{draftState.value=v as TaskState;if(v==='done')draftProgress.value=100;dirty.value=true;},
});
const curProgress=computed({get:()=>draftProgress.value,set:(v:number)=>{draftProgress.value=v;dirty.value=true;}});
async function applyTask(){
  const t=current.value;if(!t)return;
  if(await core.mutate('update',{id:t.id,record:{title:t.title,state:draftState.value,progress:draftProgress.value}},draftRevision.value)){
    dirty.value=false;draftRevision.value=core.revision.value;
  }
}
async function toggle(t:Task){await core.mutate('update',{id:t.id,record:{title:t.title,state:t.done?'queued':'done',progress:t.done?0:100}});}
async function add(){
  const title=newTitle.value.trim();if(!title)return;
  if(await core.mutate('create',{record:{title,state:'queued',progress:0}})){
    newTitle.value='';state.selected=core.items.value[core.items.value.length-1]?.id??'';
  }
}
async function remove(t:Task){await core.mutate('delete',{id:t.id});}
function push(){
  const t=current.value;
  if(t)void eye.setScene(props.type,eye.sceneStyle,{title:t.title,progress:t.progress/100,task_state:t.state});
}
function hide(){void eye.setScene(null);}
</script>

<template>
  <div class="feature" :class="ff">
    <NkHeader icon="task_alt" title="任务" :subtitle="subtitle" :tone="active ? 'ok' : 'default'" />
    <p v-if="core.error.value" role="alert" class="muted">{{ core.error.value }}</p>
    <div class="grid">
      <section class="card summary">
        <NkProgressRing :value="ratio" :size="ff === 'phone' ? 160 : 200" :stroke="12" :tone="ratio >= 1 && total ? 'ok' : 'primary'">
          <div class="ring-inner">
            <span class="big mono">{{ doneCount }}<span class="muted small">/{{ total }}</span></span>
            <span class="muted">已完成</span>
          </div>
        </NkProgressRing>
        <div v-if="current" class="cur">
          <div class="cur-title">{{ current.title }}</div>
          <NkSegmentRow v-model="curState" title="状态" :items="STATES" stacked />
          <NkSliderRow v-model="curProgress" title="进度" unit="%" :disabled="current.state === 'done'" />
          <MdButton variant="tonal" :disabled="!dirty || core.busy.value || !core.available.value" @click="applyTask">{{ core.busy.value ? '保存中…' : '保存状态' }}</MdButton>
          <span v-if="dirty" class="muted">编辑值尚未保存到设备</span>
        </div>
        <p v-else class="muted">新建一个待办后可推送到眼睛。</p>
        <NkActionBar primary-text="推送当前任务" primary-icon="visibility" secondary-text="隐藏" secondary-icon="close" :disabled="!current" @primary="push" @secondary="hide" />
      </section>
      <section class="card">
        <NkListSection title="待办" :card="false">
          <div v-for="t in state.tasks" :key="t.id" class="task" :class="{ sel: t.id === state.selected, done: t.done }">
            <button type="button" class="check" :disabled="core.busy.value" :aria-label="t.done ? '标记未完成' : '标记完成'" @click="toggle(t)">
              <UiIcon :name="t.done ? 'check_circle' : 'check'" :size="22" />
            </button>
            <button type="button" class="task-main" @click="state.selected = t.id">
              <span class="task-title">{{ t.title }}</span>
              <span class="task-sub">{{ STATES.find((s) => s.id === t.state)?.label }} · {{ t.progress }}%</span>
            </button>
            <button type="button" class="check" :disabled="core.busy.value" aria-label="删除" @click="remove(t)"><UiIcon name="delete" :size="20" /></button>
          </div>
          <p v-if="!total" class="muted">还没有待办。</p>
        </NkListSection>
        <div class="row add">
          <MdTextField v-model="newTitle" label="新建待办" placeholder="要做什么" @keydown.enter="add" />
          <MdButton variant="tonal" :disabled="!newTitle.trim() || core.busy.value || !core.available.value" @click="add"><UiIcon name="add" :size="20" />添加</MdButton>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.feature { display: flex; flex-direction: column; gap: 16px; }
.grid { display: grid; grid-template-columns: 1fr; gap: 16px; }
.feature.desktop .grid, .feature.tablet .grid { grid-template-columns: 1fr 1fr; }
.card {
  display: flex; flex-direction: column; gap: 16px;
  padding: 16px; border-radius: var(--radius-l);
  background: var(--md-surface-container); color: var(--md-on-surface);
}
.summary { align-items: center; }
.ring-inner { display: flex; flex-direction: column; align-items: center; }
.big { font-size: 40px; font-weight: 700; }
.small { font-size: 18px; font-weight: 500; }
.cur { width: 100%; display: flex; flex-direction: column; gap: 6px; }
.cur-title { font-weight: 700; font-size: 16px; padding: 0 12px; }
.task { display: flex; align-items: center; border-radius: var(--radius-m); }
.task.sel { background: var(--md-secondary-container); color: var(--md-on-secondary-container); }
.task.done .task-title { text-decoration: line-through; opacity: 0.6; }
.check {
  width: 44px; height: 44px; border: 0; background: transparent; color: inherit; cursor: pointer;
  display: inline-flex; align-items: center; justify-content: center; border-radius: 50%;
}
.task.done .check { color: var(--md-primary); }
.task-main {
  flex: 1; min-width: 0; min-height: 52px; padding: 8px 6px; border: 0; background: transparent; color: inherit;
  display: flex; flex-direction: column; align-items: flex-start; gap: 2px; text-align: left; cursor: pointer;
}
.task-title { font-weight: 600; }
.task-sub { font-size: 12px; opacity: 0.75; }
.add { align-items: flex-end; }
.add > :first-child { flex: 1; }
</style>
