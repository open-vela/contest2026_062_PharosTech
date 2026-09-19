/* Feature registry: eye.scene types -> consumer feature pages. Files are
 * <Type>Feature.vue in this folder (kebab types map to PascalCase). `stage`
 * splits the list into what works today and what waits for hardware. */
import type { FeatureDef } from './contract';

const p = (payload: Record<string, unknown> | null, k: string) => (payload && typeof payload[k] === 'string' ? (payload[k] as string) : null);

export const FEATURES: FeatureDef[] = [
  { type: 'music', stage: 'ready', label: '音乐', icon: 'music_note', group: '媒体', blurb: '播放控制与歌词', load: () => import('./MusicFeature.vue'), mini: () => import('./MusicMini.vue'), status: (pl, a) => (a ? p(pl, 'title') ?? '播放中' : null) },
  { type: 'audio', stage: 'ready', label: '音频路由', icon: 'volume_up', group: '媒体', blurb: '输出设备与音量', load: () => import('./AudioFeature.vue'), mini: () => import('./AudioMini.vue') },
  { type: 'eq', stage: 'planned', needs: '需要音频 DSP / 均衡器驱动', label: '均衡器', icon: 'tune', group: '媒体', blurb: '音效预设与频段', load: () => import('./EqFeature.vue'), mini: () => import('./EqMini.vue') },
  { type: 'subwoofer', stage: 'planned', needs: '需要 2.1 功放与分频驱动', label: '低音炮', icon: 'subwoofer', group: '媒体', blurb: '低频增强', load: () => import('./SubwooferFeature.vue'), mini: () => import('./SubwooferMini.vue') },
  { type: 'caption', stage: 'ready', label: '字幕', icon: 'article', group: '媒体', blurb: '实时字幕显示', load: () => import('./CaptionFeature.vue'), mini: () => import('./CaptionMini.vue'), status: (pl, a) => (a ? p(pl, 'current_line') : null) },
  { type: 'timer', stage: 'ready', label: '倒计时', icon: 'timer', group: '时间', blurb: '拨盘设定与预设', load: () => import('./TimerFeature.vue'), mini: () => import('./TimerMini.vue') },
  { type: 'stopwatch', stage: 'ready', label: '秒表', icon: 'schedule', group: '时间', blurb: '计时与分段', load: () => import('./StopwatchFeature.vue'), mini: () => import('./StopwatchMini.vue') },
  { type: 'alarm', stage: 'ready', label: '闹钟', icon: 'alarm', group: '时间', blurb: '多闹钟与重复', load: () => import('./AlarmFeature.vue'), mini: () => import('./AlarmMini.vue') },
  { type: 'calendar', stage: 'ready', label: '日历', icon: 'calendar', group: '时间', blurb: '日程与倒数日', load: () => import('./CalendarFeature.vue'), mini: () => import('./CalendarMini.vue') },
  { type: 'sleep-timer', stage: 'ready', label: '睡眠定时', icon: 'moon', group: '时间', blurb: '定时休眠', load: () => import('./SleepTimerFeature.vue'), mini: () => import('./SleepTimerMini.vue') },
  { type: 'call', stage: 'planned', needs: '需要通话协议栈与麦克风阵列', label: '通话', icon: 'call', group: '通讯', blurb: '联系人与来电', load: () => import('./CallFeature.vue'), mini: () => import('./CallMini.vue') },
  { type: 'presence', stage: 'planned', needs: '需要毫米波 / 摄像头在场检测', label: '在场', icon: 'presence', group: '通讯', blurb: '谁在家', load: () => import('./PresenceFeature.vue'), mini: () => import('./PresenceMini.vue') },
  { type: 'companion', stage: 'ready', label: '陪伴', icon: 'companion', group: '通讯', blurb: '陪伴模式', load: () => import('./CompanionFeature.vue'), mini: () => import('./CompanionMini.vue') },
  { type: 'weather', stage: 'ready', label: '天气', icon: 'cloud', group: '信息', blurb: '城市与预报', load: () => import('./WeatherFeature.vue'), mini: () => import('./WeatherMini.vue') },
  { type: 'briefing', stage: 'ready', label: '简报', icon: 'article', group: '信息', blurb: '每日播报', load: () => import('./BriefingFeature.vue'), mini: () => import('./BriefingMini.vue') },
  { type: 'task', stage: 'ready', label: '任务', icon: 'task_alt', group: '信息', blurb: '待办与进度', load: () => import('./TaskFeature.vue'), mini: () => import('./TaskMini.vue'), status: (pl, a) => (a ? p(pl, 'title') : null) },
  { type: 'memory', stage: 'ready', label: '记忆', icon: 'memory', group: '信息', blurb: '记住的事', load: () => import('./MemoryFeature.vue'), mini: () => import('./MemoryMini.vue') },
  { type: 'health', stage: 'planned', needs: '需要心率传感器', label: '健康', icon: 'heart_rate', group: '信息', blurb: '心率与提醒', load: () => import('./HealthFeature.vue'), mini: () => import('./HealthMini.vue') },
  { type: 'battery', stage: 'planned', needs: '需要电量计与充电芯片驱动', label: '电量', icon: 'battery', group: '系统', blurb: '电量与充电', load: () => import('./BatteryFeature.vue'), mini: () => import('./BatteryMini.vue') },
  { type: 'network', stage: 'ready', label: '网络', icon: 'wifi', group: '系统', blurb: 'WiFi 与连接', load: () => import('./NetworkFeature.vue'), mini: () => import('./NetworkMini.vue') },
  { type: 'devices', stage: 'ready', label: '设备', icon: 'devices', group: '系统', blurb: '已接入硬件', load: () => import('./DevicesFeature.vue'), mini: () => import('./DevicesMini.vue') },
  { type: 'system', stage: 'ready', label: '系统', icon: 'system', group: '系统', blurb: '系统状态', load: () => import('./SystemFeature.vue'), mini: () => import('./SystemMini.vue') },
  { type: 'privacy', stage: 'planned', needs: '需要摄像头 / 麦克风硬件开关状态', label: '隐私', icon: 'privacy', group: '系统', blurb: '摄像头与麦克风', load: () => import('./PrivacyFeature.vue'), mini: () => import('./PrivacyMini.vue') },
  { type: 'identity', stage: 'planned', needs: '需要人脸 / 声纹识别', label: '身份', icon: 'identity', group: '系统', blurb: '认主与家人', load: () => import('./IdentityFeature.vue'), mini: () => import('./IdentityMini.vue') },
  { type: 'home', stage: 'planned', needs: '需要家居网关协议接入', label: '家居', icon: 'home', group: '系统', blurb: '家居联动', load: () => import('./HomeFeature.vue'), mini: () => import('./HomeMini.vue') },
  { type: 'sleep', stage: 'ready', label: '休眠', icon: 'moon', group: '系统', blurb: '休眠与唤醒', load: () => import('./SleepFeature.vue'), mini: () => import('./SleepMini.vue') },
  { type: 'pairing', stage: 'ready', label: '配对', icon: 'qr_code', group: '系统', blurb: '扫码接入说明', load: () => import('./PairingFeature.vue'), mini: () => import('./PairingMini.vue') },
];

export const FEATURE_GROUPS = ['媒体', '时间', '通讯', '信息', '系统'];
export const featureByType = (type: string): FeatureDef | undefined => FEATURES.find((f) => f.type === type);
/** Device scenes that belong to a feature under another name. */
const SCENE_ALIAS: Record<string, string> = { qr: 'pairing' };
/** Feature that owns a scene reported by the device (eye.activeScene). */
export const featureByScene = (scene: string): FeatureDef | undefined => featureByType(SCENE_ALIAS[scene] ?? scene);
