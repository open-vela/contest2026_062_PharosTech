import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useProductRecords } from './useProductRecords';
import { usePageVisible } from './usePageVisible';
import { useEyeScene } from './useEyeScene';
import { sleepTimerScene, stopwatchScene, timerScene } from './eyeScenePayload';
import { useEyeStore } from '../stores/eye';
import { useSessionStore } from '../stores/session';
import type { ProductRecord } from '../stores/product';

export interface CoreTimer extends ProductRecord {
  id:string; kind:'countdown'|'stopwatch'|'sleep'; label:string;
  status:'running'|'paused'|'finished'|'waiting-clock';
  duration_ms:number; remaining_ms:number; elapsed_ms:number; laps:number[];
  recovery?:string;
}

const SCENE:Record<CoreTimer['kind'],string>={countdown:'timer',stopwatch:'stopwatch',sleep:'sleep-timer'};

export function useCoreTimer(kind:CoreTimer['kind']) {
  /* Core owns the clock; between two snapshots the view extrapolates locally,
   * so the list is only re-read every 2 s (shared by all timer cards) to learn
   * about pause / finish / changes made elsewhere. */
  const core=useProductRecords<CoreTimer>('timer',2000);
  const visible=usePageVisible();
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
  const running=computed(()=>current.value?.status==='running');
  const finished=computed(()=>current.value?.status==='finished');
  const waitingClock=computed(()=>current.value?.status==='waiting-clock');
  /* View clock: 50 ms while something counts on screen, 1 s while the tab is
   * hidden but the timer is on the eyes (the device does not count by itself),
   * off otherwise. */
  let animation:ReturnType<typeof setInterval>|undefined;
  const clock=(every:number)=>{
    if(animation)clearInterval(animation);
    now.value=performance.now();
    animation=every?setInterval(()=>{now.value=performance.now();},every):undefined;
  };
  onBeforeUnmount(()=>clock(0));
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

  /* Eye link. The device draws the number it is given and does not count by
   * itself, so the selected timer is re-sent while it is on the eyes: once a
   * second for countdown/stopwatch, once a minute for the sleep timer. */
  const scene=useEyeScene(SCENE[kind],()=>{
    const t=current.value;
    if(kind==='stopwatch')return stopwatchScene({elapsedMs:elapsed.value,running:running.value,label:t?.label});
    if(!t||waitingClock.value)return null;
    if(kind==='sleep')return finished.value?null:sleepTimerScene({remainingMs:remaining.value,running:running.value});
    return timerScene({durationMs:t.duration_ms,remainingMs:remaining.value,running:running.value,finished:finished.value,label:t.label});
  },{hideWhenEmpty:true});
  watch([running,visible,scene.held],([counting,shown,held])=>clock(!counting?0:shown?50:held?1000:0),{immediate:true});

  /* Sleep timer end. Core only marks the record `finished`; until it acts on
   * that itself, a panel that watched the timer run out puts the eyes to sleep
   * and pauses playback. */
  if(kind==='sleep'){
    const eye=useEyeStore();
    const session=useSessionStore();
    let sawRunning='';
    watch(()=>[current.value?.id??'',current.value?.status??''] as const,([id,status])=>{
      if(status==='running'){sawRunning=id;return;}
      if(status!=='finished'){sawRunning='';return;}
      if(sawRunning!==id||!session.canControl)return;
      sawRunning='';
      void eye.setMode('sleep');
      if(session.isOwner&&session.client?.capabilities.includes('core.media-v1')){
        void session.request('music.pause',{}).catch(()=>undefined);
      }
    },{immediate:true});
  }
  return {core,selected,choices,select,current,running,finished,waitingClock,remaining,elapsed,create,action,scene};
}
