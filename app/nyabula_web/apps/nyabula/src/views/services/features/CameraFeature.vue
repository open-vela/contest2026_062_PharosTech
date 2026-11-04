<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { MdButton, UiIcon } from '@nyabula/ui';
import { FeaturePageHeader } from '../featureHeader';
import CameraCalibrationEyes from './CameraCalibrationEyes.vue';
import CameraIntercom from './CameraIntercom.vue';
import { useVisionStore } from '../../../stores/vision';
import { useVisiblePoll } from '../../../composables/usePageVisible';
import { useCamera } from '../../../composables/useCamera';
import type { CalibrationPoint } from '../../../lib/cameraMedia';
import type { FeatureProps } from './contract';

defineProps<FeatureProps>();
const camera = useCamera();
const vision = useVisionStore();
useVisiblePoll(() => void vision.refresh(), 500);
async function startVision(follow: boolean) {
  if (await vision.start(follow)) { if (!streaming.value && canPreview) await camera.start(); }
}
const { video, status, calibration, fit, busy, streaming, received, error, readError, photoUrl,
  canUse, canPreview, label } = camera;
const learningSetup=ref(false);
const learnBox=ref({x:.375,y:1/3,width:.25,height:1/3});
async function prepareLearning(){
  if(vision.state?.running&&!await vision.stop())return;
  learningSetup.value=true;
  if(!streaming.value&&canPreview)await camera.start();
}
async function learnObject(){if(await vision.learn(learnBox.value,!!calibration.value?.calibrated))learningSetup.value=false;}
const calibrating = ref(false), saved = ref(false), recording = ref(false);
const points = ref<CalibrationPoint[]>([]);
const target = ref<{ u: number; v: number }>();
const gazeX = ref(0), gazeY = ref(0), gazeTouched = ref(false);
const pointNames = ['左上', '右上', '右下', '左下', '中心'];
const hasImage = computed(() => streaming.value ? received.value > 0 : !!photoUrl.value);
const step = computed(() => Math.min(points.value.length, 4));
let aimTimer: ReturnType<typeof setTimeout> | undefined;
const aiming = ref(false);
let pendingAim = false, hasHeldAim = false;
let holdTimer: ReturnType<typeof setInterval> | undefined;

async function sendAim() {
  if (aiming.value) { pendingAim = true; return; }
  aiming.value = true; pendingAim = false;
  try { await camera.aim(gazeX.value / 100, gazeY.value / 100); }
  finally { aiming.value = false; if (pendingAim && calibrating.value) aimTimer = setTimeout(sendAim, 80); }
}
function onCalibrationLook(value: { x?: number; y?: number; release?: boolean }) {
  if (value.release || !canUse.value || recording.value || points.value.length >= 5 ||
      typeof value.x !== 'number' || typeof value.y !== 'number') return;
  gazeX.value = value.x * 100; gazeY.value = value.y * 100;
  gazeTouched.value = true; hasHeldAim = true; saved.value = false;
  void sendAim();
}
function selectPoint(event: MouseEvent) {
  if (!hasImage.value || !canUse.value) return;
  const bounds = (event.currentTarget as HTMLElement).getBoundingClientRect();
  const u = Math.max(0, Math.min(1, (event.clientX - bounds.left) / bounds.width));
  const v = Math.max(0, Math.min(1, (event.clientY - bounds.top) / bounds.height));
  if(learningSetup.value){learnBox.value={...learnBox.value,x:Math.max(0,Math.min(1-learnBox.value.width,u-learnBox.value.width/2)),y:Math.max(0,Math.min(1-learnBox.value.height,v-learnBox.value.height/2))};return;}
  if(vision.state?.running&&!calibrating.value)return;
  target.value = { u, v };
  if (!calibrating.value) void camera.point(u, v);
}
async function beginCalibration() {
  if (vision.state?.running && !await vision.stop()) return;
  learningSetup.value=false;
  calibrating.value = true; saved.value = false; points.value = []; target.value = undefined; fit.value = undefined;
  gazeX.value = gazeY.value = 0; gazeTouched.value = false;
  if (!streaming.value && canPreview) void camera.start();
  if (holdTimer) clearInterval(holdTimer);
  holdTimer = setInterval(() => {
    if (calibrating.value && hasHeldAim && !aiming.value && !recording.value && !busy.value) void sendAim();
  }, 1000);
}
function cancelCalibration(release = true) {
  calibrating.value = false; pendingAim = false;
  if (holdTimer) clearInterval(holdTimer);
  holdTimer = undefined;
  if (release && hasHeldAim) void camera.aim(0, 0, 0);
  hasHeldAim = false;
  if (aimTimer) clearTimeout(aimTimer);
}
async function recordPoint() {
  if (!target.value || !gazeTouched.value || points.value.length >= 5 || recording.value || aiming.value || !canUse.value) return;
  const point = { ...target.value, x: gazeX.value / 100, y: gazeY.value / 100 };
  recording.value = true;
  if (aimTimer) clearTimeout(aimTimer);
  try {
    if (!await camera.aim(point.x, point.y)) return;
    points.value.push(point);
    target.value = undefined; gazeTouched.value = false;
    if (points.value.length === 5) await camera.calibrate(points.value, false);
  } finally { recording.value = false; }
}
async function saveCalibration() {
  if (await camera.calibrate(points.value, true)) { saved.value = true; cancelCalibration(); }
}
function retryPoint() {
  points.value.pop(); fit.value = undefined; target.value = undefined; gazeTouched.value = false;
}
watch(camera.connection, () => cancelCalibration(false));
onBeforeUnmount(() => cancelCalibration());
</script>

<template>
  <div class="camera-feature">
    <FeaturePageHeader title="摄影" icon="camera" :subtitle="label" :tone="status?.phase === 'error' ? 'error' : streaming ? 'ok' : 'default'" />
    <div v-if="error || readError" class="camera-error" role="alert">
      <span>{{ error || readError }}</span><button type="button" @click="camera.refresh">重新读取</button>
    </div>
    <div class="camera-layout" :class="{ calibrating }">
      <section class="camera-view ui-panel">
        <div class="view-heading"><span class="live-status" :class="{ live: streaming && received > 0 }"><i />{{ label }}</span><span>640 × 480</span></div>
        <div v-if="!calibrating" class="camera-actions">
          <MdButton v-if="!streaming" :disabled="!canUse || !canPreview" @click="camera.start"><UiIcon name="play_arrow" :size="18" />开启画面</MdButton>
          <MdButton v-else variant="outlined" @click="camera.stop"><UiIcon name="stop" :size="18" />停止预览</MdButton>
          <MdButton variant="tonal" :disabled="!canUse" @click="camera.photograph"><UiIcon name="camera" :size="18" />{{ busy ? '处理中…' : '拍照' }}</MdButton>
          <a v-if="photoUrl" class="photo-download" :href="photoUrl" download="Nyabula-photo.png"><UiIcon name="download" :size="18" />保存照片</a>
        </div>
        <div class="viewfinder" :class="{ selectable: hasImage && (calibrating || calibration?.calibrated) }" @click="selectPoint">
          <video v-show="streaming && received > 0" ref="video" muted playsinline autoplay aria-label="摄像头实时画面" />
          <img v-if="!streaming && photoUrl" :src="photoUrl" alt="刚拍下的摄像头画面" />
          <div v-if="!hasImage" class="empty-view">
            <div class="lens-mark"><UiIcon name="camera" :size="38" /></div>
            <strong>{{ streaming ? '正在打开摄像头' : '从猫猫的视角看看' }}</strong>
            <p>{{ streaming ? '等待设备送来第一帧画面' : '主动开启画面或拍照，退出页面后停止预览。' }}</p>
          </div>
          <div v-if="streaming && received > 0 && !calibrating" class="vision-overlay" aria-hidden="true"><span v-for="(face, index) in vision.faces" :key="index" class="face-box" :style="{left:`${face.x*100}%`,top:`${face.y*100}%`,width:`${face.width*100}%`,height:`${face.height*100}%`}"><b>人脸 {{ Math.round(face.confidence*100) }}%</b></span></div>
          <div v-if="hasImage&&!calibrating&&(learningSetup||vision.target)" class="vision-overlay" aria-hidden="true"><span class="face-box object-box" :class="{learning:learningSetup||vision.state?.phase==='learning'}" :style="{left:`${(learningSetup?learnBox.x:vision.target!.x)*100}%`,top:`${(learningSetup?learnBox.y:vision.target!.y)*100}%`,width:`${(learningSetup?learnBox.width:vision.target!.width)*100}%`,height:`${(learningSetup?learnBox.height:vision.target!.height)*100}%`}"><b>{{ learningSetup?'把物品放进框内':vision.state?.phase==='learning'?'学习中':'物品' }}</b></span></div>
          <span v-if="hasImage && target && !learningSetup && !vision.state?.running" class="target-cross" :style="{ left: `${target.u * 100}%`, top: `${target.v * 100}%` }"><i /><b /></span>
          <span v-for="(point, index) in calibrating ? points : []" :key="index" class="saved-point" :style="{ left: `${point.u * 100}%`, top: `${point.v * 100}%` }">{{ index + 1 }}</span>
          <div v-if="hasImage" class="frame-label">{{ calibrating ? `校准 · ${pointNames[step]}` : photoUrl && !streaming ? '本次拍摄' : '实时画面' }}</div>
        </div>
        <p v-if="!calibrating && !canPreview" class="camera-hint">当前浏览器不支持实时预览，可拍照查看。</p>
        <p v-else-if="!calibrating" class="camera-hint">{{ hasImage && !calibrating && calibration?.calibrated ? '点击画面中的目标，让屏幕猫眼望向它。' : '画面经当前已登录的设备连接传输，拍照不会自动发给云端模型。' }}</p>
      </section>

      <section class="calibration-panel ui-panel">
        <div class="section-heading"><div><span class="eyebrow">屏幕猫眼</span><h2>看向同一个地方</h2></div><UiIcon name="visibility" :size="24" /></div>
            <CameraCalibrationEyes v-if="calibrating" :disabled="!canUse || recording || points.length >= 5" @look="onCalibrationLook" />
            <div v-if="calibrating" class="calibration-aim"><span>{{ gazeTouched ? '已锁定注视点' : '点击下方画布，选定并保持注视方向' }}</span><code v-if="gazeTouched">{{ (gazeX / 100).toFixed(2) }} / {{ (gazeY / 100).toFixed(2) }}</code></div>
        <template v-if="!calibrating">
          <p class="calibration-intro">用四角和中心五个点，校准画面坐标与猫眼注视方向。只控制屏幕猫眼。</p>
          <div v-if="calibration?.calibrated" class="calibration-saved"><UiIcon name="check_circle" :size="19" /><div><strong>{{ saved ? '校准已保存' : '已有五点校准' }}</strong><span>拟合误差 {{ calibration.rmsError?.toFixed(3) }} · 点击画面试一试</span></div></div>
          <MdButton variant="outlined" :disabled="!canUse || (!canPreview && !hasImage)" @click="beginCalibration">{{ calibration?.calibrated ? '重新校准' : '开始五点校准' }}</MdButton>

        </template>
        <template v-else>
          <ol class="calibration-steps" aria-label="五点校准进度"><li v-for="(name, index) in pointNames" :key="name" :class="{ done: index < points.length, current: index === points.length }"><span>{{ index < points.length ? '✓' : index + 1 }}</span>{{ name }}</li></ol>
          <template v-if="points.length < 5">
            <p class="calibration-instruction">在画面的<strong>{{ pointNames[step] }}</strong>区域点选一个目标，再点击下方眼睛画布，锁定猫眼方向后记录。</p>
            <div class="point-readout">{{ target ? `画面目标 ${target.u.toFixed(2)} / ${target.v.toFixed(2)}` : '请先在画面中点选目标' }}</div>
            <MdButton :disabled="!target || !gazeTouched || !canUse || recording || aiming" @click="recordPoint">记录{{ pointNames[step] }}点</MdButton>
          </template>
          <div v-else-if="fit" class="fit-result" :class="{ warning: !fit.acceptable }">
            <strong>{{ fit.acceptable ? '五点拟合完成' : '五点偏差较大，请重新校准' }}</strong>
            <dl><div><dt>平均误差</dt><dd>{{ fit.rmsError?.toFixed(3) }}</dd></div><div><dt>最大误差</dt><dd>{{ fit.maxError?.toFixed(3) }}</dd></div></dl>
            <p>误差以猫眼注视坐标计；保存后请在实际使用距离下点击画面检查。</p>
            <MdButton :disabled="!fit.acceptable || !canUse" @click="saveCalibration">保存校准</MdButton>
          </div>
          <div class="calibration-footer"><button v-if="points.length" type="button" :disabled="busy || recording" @click="retryPoint">重做上一点</button><button type="button" :disabled="busy || recording" @click="cancelCalibration()">取消</button></div>
        </template>
      </section>
    </div>
    <section v-if="!calibrating" class="vision-panel ui-panel">
      <div class="section-heading"><div><span class="eyebrow">本地视觉</span><h2>{{ learningSetup?'让猫猫记住眼前物品':vision.label }}</h2></div><UiIcon name="presence" :size="24" /></div>
      <div v-if="!learningSetup" class="vision-actions">
        <template v-if="!vision.state?.running"><MdButton :disabled="!vision.canWrite" @click="startVision(false)">开启人脸检测</MdButton><MdButton variant="outlined" :disabled="!vision.canWrite || !calibration?.calibrated" @click="startVision(true)">猫眼跟随人脸</MdButton></template>
        <MdButton v-else variant="outlined" :disabled="!vision.canWrite" @click="vision.stop">停止分析</MdButton>
      </div>
      <div v-if="vision.trackerSupported" class="object-learning">
        <template v-if="learningSetup"><p class="camera-hint">把物品放入框内，点击画面可移动框的位置。开始后缓慢转动，保持约三秒。</p><label class="camera-hint">框的大小 <input v-model.number="learnBox.width" type="range" min="0.10" max="0.40" step="0.025" @input="learnBox.height=learnBox.width*4/3;learnBox.x=Math.min(learnBox.x,1-learnBox.width);learnBox.y=Math.min(learnBox.y,1-learnBox.height)"/></label><div class="vision-actions"><MdButton :disabled="!vision.canWrite||!hasImage" @click="learnObject">学习这个物品</MdButton><MdButton variant="text" @click="learningSetup=false">取消</MdButton></div></template>
        <MdButton v-else-if="!vision.state?.running" variant="tonal" :disabled="!vision.canWrite||!canPreview" @click="prepareLearning">学习一个物品</MdButton>
        <p v-if="vision.state?.tracking&&vision.state.running" class="camera-hint">{{vision.state.phase==='lost'?'请把原物品带回画面，连续确认后恢复跟随。':'外观模板只用于本次追踪，停止后清除。'}}{{!calibration?.calibrated?' 尚未校准，当前仅显示追踪框。':''}}</p>
      </div>
      <p class="camera-hint">{{ !calibration?.calibrated ? '先完成五点校准，再让猫眼自动看向目标。' : '检测在设备本地运行；离开页面仍继续，可在此停止或关闭摄像头隐私开关。' }}</p>
      <p v-if="vision.error || vision.readError || vision.state?.followError" class="camera-error" role="alert">{{ vision.error || vision.readError || `猫眼映射未就绪（${vision.state?.followError}），请检查五点校准。` }}</p>
    </section>
    <CameraIntercom />
  </div>
</template>

<style scoped>
.camera-feature{display:grid;gap:20px}.camera-layout{display:grid;grid-template-columns:minmax(0,1.45fr) minmax(285px,1fr);gap:20px;align-items:start}.camera-layout.calibrating{grid-template-columns:minmax(0,1fr);max-width:860px;margin-inline:auto;width:100%}.calibrating .viewfinder{width:min(100%,360px);margin-inline:auto}.calibrating .view-heading{padding:9px 16px}.calibrating .calibration-panel{gap:12px;padding:16px}.camera-view{overflow:hidden}.view-heading{display:flex;justify-content:space-between;gap:12px;padding:16px 20px;font-size:13px;color:var(--md-on-surface-variant)}.live-status{display:flex;align-items:center;gap:8px}.live-status i{width:7px;height:7px;border-radius:50%;background:var(--md-outline)}.live-status.live i{background:var(--md-primary);box-shadow:0 0 10px color-mix(in srgb,var(--md-primary) 45%,transparent)}.viewfinder{position:relative;aspect-ratio:4/3;background:#101617;color:#dde8e5;overflow:hidden}.viewfinder.selectable{cursor:crosshair}.viewfinder video,.viewfinder>img{position:absolute;width:100%;height:100%;object-fit:contain;inset:0}.empty-view{height:100%;display:flex;align-items:center;justify-content:center;flex-direction:column;gap:14px;text-align:center;padding:28px;background:radial-gradient(ellipse at 50% 45%,#263632 0,transparent 60%)}.lens-mark{width:84px;height:84px;border-radius:50%;border:1px solid #55786a55;display:grid;place-items:center;box-shadow:0 0 0 14px #52796b0b;color:#9abcb0;margin-bottom:10px}.empty-view strong{font-size:18px;font-weight:500}.empty-view p{max-width:260px;font-size:14px;line-height:1.7;color:#a8bab3;margin:0}.frame-label{position:absolute;left:14px;bottom:14px;background:#0008;color:#fff;border-radius:6px;padding:6px 10px;font-size:12px;pointer-events:none}.target-cross{position:absolute;width:32px;height:32px;transform:translate(-50%,-50%);border:1px solid #fff;border-radius:50%;box-shadow:0 0 0 1px #0008;pointer-events:none}.target-cross i,.target-cross b{position:absolute;background:white;box-shadow:0 0 2px #000}.target-cross i{width:42px;height:1px;top:15px;left:-6px}.target-cross b{width:1px;height:42px;left:15px;top:-6px}.saved-point{position:absolute;transform:translate(-50%,-50%);display:grid;place-items:center;width:25px;height:25px;border-radius:50%;background:var(--md-primary);color:var(--md-on-primary);font:600 12px var(--font-body);pointer-events:none}.camera-actions{display:flex;align-items:center;gap:10px;padding:0 20px 16px;flex-wrap:wrap}.camera-actions :deep(.md-btn){display:inline-flex;align-items:center;gap:8px}.photo-download{display:flex;align-items:center;gap:6px;padding:10px 0;color:var(--md-primary);font-size:14px;text-decoration:none;margin-left:auto}.camera-hint{font-size:13px;line-height:1.7;color:var(--md-on-surface-variant);margin:8px 0 0}.camera-view>.camera-hint{padding:0 20px 18px}.calibration-panel{display:grid;gap:18px;padding:22px}.section-heading{display:flex;align-items:center;justify-content:space-between;gap:12px}.eyebrow{font-size:12px;color:var(--md-on-surface-variant);letter-spacing:.06em}.section-heading h2{margin:6px 0 0;font-size:19px;font-weight:600}.section-heading>.ui-icon{color:var(--md-primary)}.calibration-intro,.calibration-instruction{font-size:14px;line-height:1.8;margin:0;color:var(--md-on-surface-variant)}.calibration-instruction strong{color:var(--md-on-surface);padding:0 4px}.calibration-saved{display:flex;align-items:flex-start;gap:9px;padding-top:16px;border-top:1px solid var(--md-outline-variant)}.calibration-saved>.ui-icon{color:var(--md-primary)}.calibration-saved div{display:grid;gap:5px}.calibration-saved strong{font-size:14px}.calibration-saved span{font-size:12px;color:var(--md-on-surface-variant);line-height:1.6}.calibration-steps{display:grid;grid-template-columns:repeat(5,1fr);gap:5px;list-style:none;padding:0;margin:0}.calibration-steps li{display:grid;justify-items:center;gap:7px;font-size:12px;color:var(--md-on-surface-variant)}.calibration-steps li>span{display:grid;place-items:center;width:28px;height:28px;border-radius:50%;border:1px solid var(--md-outline-variant);font-size:12px}.calibration-steps .current{color:var(--md-primary)}.calibration-steps .current>span{border-color:var(--md-primary);background:var(--md-primary-container)}.calibration-steps .done>span{background:var(--md-primary);color:var(--md-on-primary);border-color:transparent}.point-readout{font-size:13px;padding:10px 12px;border-radius:var(--radius-s);background:var(--md-surface-container-low);color:var(--md-on-surface-variant)}.calibration-aim{display:flex;align-items:center;justify-content:space-between;gap:12px;color:var(--md-on-surface-variant);font-size:13px}.calibration-aim code{font-variant-numeric:tabular-nums}.calibration-footer{display:flex;justify-content:space-between;gap:16px}.calibration-footer button,.camera-error button{border:0;background:transparent;color:var(--md-primary);font:500 13px var(--font-body);padding:8px 0;cursor:pointer}.fit-result{display:grid;gap:16px}.fit-result>strong{font-size:15px;color:var(--md-primary)}.fit-result.warning>strong{color:var(--md-error)}.fit-result dl{display:flex;gap:28px;margin:0}.fit-result dt{font-size:12px;color:var(--md-on-surface-variant)}.fit-result dd{font-size:24px;font-variant-numeric:tabular-nums;margin:6px 0 0}.fit-result p{font-size:13px;line-height:1.7;color:var(--md-on-surface-variant);margin:0}.camera-error{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:12px 16px;border-radius:var(--radius-m);background:var(--md-error-container);color:var(--md-on-error-container);font-size:14px}.camera-error button{color:inherit;white-space:nowrap}button:disabled{opacity:.45;cursor:default}button:focus-visible,a:focus-visible{outline:2px solid var(--md-primary);outline-offset:3px}
:global([data-skin='nebula'] .camera-view){background:var(--nb-glass)}:global([data-skin='instrument'] .camera-feature .frame-label){border-radius:2px}
@media(max-width:900px){.camera-layout{grid-template-columns:1fr}.calibration-panel{padding:20px}.camera-actions{padding:0 16px 14px}.view-heading{padding:14px 16px}.camera-view>.camera-hint{padding:0 16px 16px}}
@media(max-width:420px){.camera-actions{gap:8px}.photo-download{margin-left:0}.empty-view{padding:20px;gap:10px}.empty-view strong{font-size:16px}.lens-mark{width:64px;height:64px;margin-bottom:6px}.calibration-panel{padding:18px}}

.object-learning{display:grid;gap:10px}.object-learning input{vertical-align:middle;accent-color:var(--md-primary)}.object-box.learning{border-style:dashed;border-color:#f0ca75}.object-box.learning b{background:#66501b}.vision-panel{display:grid;gap:14px;padding:20px}.vision-actions{display:flex;gap:10px;flex-wrap:wrap}.vision-panel .camera-hint{padding:0}.vision-overlay{position:absolute;inset:0;pointer-events:none}.face-box{position:absolute;border:2px solid #65e2ac;border-radius:8px;box-sizing:border-box;box-shadow:0 0 0 1px #16392a55}.face-box b{position:absolute;top:0;left:0;transform:translateY(-100%);white-space:nowrap;background:#123529d9;color:#e9fff5;padding:2px 6px;border-radius:4px;font:600 11px var(--font-body)}
</style>
