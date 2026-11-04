/* Eye page logic shared by all three variants. */
import { computed, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import { useVisiblePoll } from '../../composables/usePageVisible';
import { featureByType } from '../services/features';
import { MODES, SCENES, type EyeEngine } from '@nyabula/eye-engine';
import { useSessionStore } from '../../stores/session';
import { MODE_LABELS, SCENE_GROUPS, SCENE_META, useEyeStore } from '../../stores/eye';

export function useEyePage() {
  const session = useSessionStore();
  const eye = useEyeStore();
  const router = useRouter();
  const catalog = ref<Record<string,{available:boolean;error:number;source:string}>>({});
  const catalogError = ref('');
  let catalogBusy=false,generation=0;
  async function refreshCatalog() {
    if(!session.connected||!session.canControl||!eye.nativeSceneData||catalogBusy)return;
    const version=generation;catalogBusy=true;
    try {
      const result=await session.request('scene.list', {utcOffsetMinutes:-new Date().getTimezoneOffset()}) as unknown as {items:{scene:string;available:boolean;error:number;source:string}[]};
      if(version===generation){catalog.value=Object.fromEntries(result.items.map(item=>[item.scene,item]));catalogError.value='';}
    } catch(e){if(version===generation)catalogError.value=e instanceof Error?e.message:String(e);}
    finally {if(version===generation)catalogBusy=false;}
  }
  watch(()=>session.client,()=>{generation++;catalog.value={};catalogBusy=false;catalogError.value='';});
  watch(()=>[session.connected,eye.nativeSceneData],()=>void refreshCatalog(),{immediate:true});
  useVisiblePoll(()=>void refreshCatalog(),10000);
  function sceneInfo(type:string) {
    const feature=featureByType(type);
    if(feature?.stage==='planned')return {blocked:true,needsSetup:false,hint:feature.needs??'需要硬件或服务接入'};
    if(type==='pairing')return {blocked:false,needsSetup:true,hint:'打开设备配对说明'};
    if(!eye.nativeSceneData)return {blocked:false,needsSetup:false,hint:''};
    const item=catalog.value[type.replaceAll('-','_')];
    if(!item)return {blocked:true,needsSetup:false,hint:catalogError.value||'正在读取设备数据源'};
    if(item.error===-13)return {blocked:true,needsSetup:false,hint:'需要主人权限'};
    if(item.error===-95||item.error===-138||item.error===-38)return {blocked:true,needsSetup:false,hint:'设备未提供此数据源'};
    return {blocked:false,needsSetup:!item.available,hint:item.available?'设备持续同步':'先在功能页配置或添加数据'};
  }
  async function pickScene(type:string) {
    if(!session.canControl)return;
    if(eye.activeScene===type){await eye.setScene(null);return;}
    const info=sceneInfo(type);
    if(info.blocked)return;
    if(info.needsSetup){await router.push({name:'service',params:{key:session.deviceKey??'',type}});return;}
    await eye.toggleScene(type);
  }

  const pairing = ref(false);
  const pairError = ref<string | null>(null);
  let engine: EyeEngine | null = null;

  const showPairOverlay = computed(() => session.state === 'pairing-required' || (pairing.value && session.state === 'authenticating'));
  const canvasReady = computed(() => eye.ready);

  async function doPair(code: string) {
    pairing.value = true;
    pairError.value = null;
    try {
      await session.pair(code, 'Nyabula Web');
    } catch {
      pairError.value = session.lastError && /wrong pairing code|EPERM/i.test(session.lastError) ? '配对码错误' : session.lastError ?? '配对失败';
    } finally {
      pairing.value = false;
    }
  }

  function onEngineReady(e: EyeEngine) {
    engine = e;
    if (!eye.lastState?.expression) engine.setLight(eye.lightPreview / 100);
  }
  function onLook(d: { x?: number; y?: number; release?: boolean }) {
    eye.look(d);
  }
  function onLight(v: number) {
    eye.setAmbient(v);
    // On a native Core the device applies the level and its eye.state drives
    // the preview; only a link without that topic previews locally.
    if (!eye.ambientOnDevice) engine?.setLight(v / 100);
  }

  const modes = MODES.map((m) => ({ id: m, label: MODE_LABELS[m] ?? m }));
  const sceneGroups = SCENE_GROUPS.map((g) => ({
    group: g,
    scenes: SCENES.filter((s) => (SCENE_META[s]?.group ?? '系统') === g).map((s) => ({ id: s, label: SCENE_META[s]?.label ?? s, icon: SCENE_META[s]?.icon ?? 'widgets' })),
  })).filter((g) => g.scenes.length);
  const allScenes = SCENES.map((s) => ({ id: s, label: SCENE_META[s]?.label ?? s, icon: SCENE_META[s]?.icon ?? 'widgets' }));

  const stateSummary = computed(() => {
    const s = eye.lastState;
    if (!s) return null;
    return {
      mode: s.expression?.mode,
      gaze: s.gaze?.mode,
      scene: s.scene?.type ?? null,
      style: s.scene?.style,
      light: s.expression?.lightLevel,
      seq: s.seq,
    };
  });

  return { session, eye, sceneInfo, pickScene, catalogError, pairing, pairError, showPairOverlay, canvasReady, doPair, onEngineReady, onLook, onLight, modes, sceneGroups, allScenes, stateSummary };
}
