import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useProductRecords } from './useProductRecords';
import type { ProductRecord } from '../stores/product';

export interface CoreTimer extends ProductRecord {
  id:string; kind:'countdown'|'stopwatch'|'sleep'; label:string;
  status:'running'|'paused'|'finished'|'waiting-clock';
  duration_ms:number; remaining_ms:number; elapsed_ms:number; laps:number[];
  recovery?:string;
}

export function useCoreTimer(kind:CoreTimer['kind']) {
  const core=useProductRecords<CoreTimer>('timer',500);
  const selected=ref('');
  const rows=computed(()=>core.items.value.filter(t=>t.kind===kind));
  const choices=computed(()=>[{id:'new',label:'新建'},...rows.value.map(t=>({id:t.id,label:t.label||t.id}))]);
  watch([rows, core.loaded], ([list, loaded])=>{
    if(!loaded){selected.value='';return;}
    if(selected.value==='new')return;
    if(!list.some(t=>t.id===selected.value)) selected.value=[...list].reverse().find(t=>t.status!=='finished')?.id??list[list.length-1]?.id??'new';
  },{immediate:true});
  const current=computed(()=>rows.value.find(t=>t.id===selected.value)??null);
  const now=ref(performance.now());
  const animation=setInterval(()=>{now.value=performance.now();},50);
  onBeforeUnmount(()=>clearInterval(animation));
  const running=computed(()=>current.value?.status==='running');
  const finished=computed(()=>current.value?.status==='finished');
  const waitingClock=computed(()=>current.value?.status==='waiting-clock');
  const age=computed(()=>running.value?Math.max(0,now.value-core.receivedAt.value):0);
  const remaining=computed(()=>Math.max(0,(current.value?.remaining_ms??0)-age.value));
  const elapsed=computed(()=>(current.value?.elapsed_ms??0)+(kind==='stopwatch'?age.value:0));
  async function create(duration:number,label:string) {
    if(await core.mutate('create',{kind,label,duration_ms:duration})) selected.value=[...core.items.value].reverse().find(t=>t.kind===kind)?.id??'new';
  }
  async function action(operation:'pause'|'resume'|'lap'|'delete') {
    const row=current.value;if(!row)return;
    if(await core.mutate(operation,{id:row.id})&&operation==='delete')selected.value='new';
  }
  function select(value:string|string[]) {if(typeof value==='string')selected.value=value;}
  return {core,selected,choices,select,current,running,finished,waitingClock,remaining,elapsed,create,action};
}
