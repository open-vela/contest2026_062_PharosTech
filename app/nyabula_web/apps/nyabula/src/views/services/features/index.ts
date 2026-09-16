/* Feature registry: 26 eye.scene types -> consumer feature pages. Files are
 * <Type>Feature.vue in this folder (kebab types map to PascalCase). */
import type { FeatureDef } from './contract';

const p = (payload: Record<string, unknown> | null, k: string) => (payload && typeof payload[k] === 'string' ? (payload[k] as string) : null);

export const FEATURES: FeatureDef[] = [
  { type: 'music', label: '音乐', icon: 'music_note', group: '媒体', blurb: '播放控制与歌词', load: () => import('./MusicFeature.vue'), mini: () => import('./MusicMini.vue'), status: (pl, a) => (a ? p(pl, 'title') ?? '播放中' : null) },
  { type: 'audio', label: '音频路由', icon: 'volume_up', group: '媒体', blurb: '输出设备与音量', load: () => import('./AudioFeature.vue'), mini: () => import('./AudioMini.vue') },
  { type: 'eq', label: '均衡器', icon: 'tune', group: '媒体', blurb: '音效预设与频段', load: () => import('./EqFeature.vue'), mini: () => import('./EqMini.vue') },
  { type: 'subwoofer', label: '低音炮', icon: 'subwoofer', group: '媒体', blurb: '低频增强', load: () => import('./SubwooferFeature.vue'), mini: () => import('./SubwooferMini.vue') },
  { type: 'caption', label: '字幕', icon: 'article', group: '媒体', blurb: '实时字幕显示', load: () => import('./CaptionFeature.vue'), mini: () => import('./CaptionMini.vue'), status: (pl, a) => (a ? p(pl, 'text') : null) },
  { type: 'timer', label: '倒计时', icon: 'timer', group: '时间', blurb: '拨盘设定与预设', load: () => import('./TimerFeature.vue'), mini: () => import('./TimerMini.vue') },
  { type: 'stopwatch', label: '秒表', icon: 'schedule', group: '时间', blurb: '计时与分段', load: () => import('./StopwatchFeature.vue'), mini: () => import('./StopwatchMini.vue') },
  { type: 'alarm', label: '闹钟', icon: 'alarm', group: '时间', blurb: '多闹钟与重复', load: () => import('./AlarmFeature.vue'), mini: () => import('./AlarmMini.vue') },
  { type: 'calendar', label: '日历', icon: 'calendar', group: '时间', blurb: '日程与倒数日', load: () => import('./CalendarFeature.vue'), mini: () => import('./CalendarMini.vue') },
  { type: 'sleep-timer', label: '睡眠定时', icon: 'moon', group: '时间', blurb: '定时休眠', load: () => import('./SleepTimerFeature.vue'), mini: () => import('./SleepTimerMini.vue') },
  { type: 'call', label: '通话', icon: 'call', group: '通讯', blurb: '联系人与来电', load: () => import('./CallFeature.vue'), mini: () => import('./CallMini.vue'), status: (pl, a) => (a ? p(pl, 'name') : null) },
  { type: 'presence', label: '在场', icon: 'presence', group: '通讯', blurb: '谁在家', load: () => import('./PresenceFeature.vue'), mini: () => import('./PresenceMini.vue') },
  { type: 'companion', label: '陪伴', icon: 'companion', group: '通讯', blurb: '陪伴模式', load: () => import('./CompanionFeature.vue'), mini: () => import('./CompanionMini.vue') },
  { type: 'weather', label: '天气', icon: 'cloud', group: '信息', blurb: '城市与预报', load: () => import('./WeatherFeature.vue'), mini: () => import('./WeatherMini.vue'), status: (pl, a) => (a ? p(pl, 'city') : null) },
  { type: 'briefing', label: '简报', icon: 'article', group: '信息', blurb: '每日播报', load: () => import('./BriefingFeature.vue'), mini: () => import('./BriefingMini.vue') },
  { type: 'task', label: '任务', icon: 'task_alt', group: '信息', blurb: '待办与进度', load: () => import('./TaskFeature.vue'), mini: () => import('./TaskMini.vue'), status: (pl, a) => (a ? p(pl, 'title') : null) },
  { type: 'memory', label: '记忆', icon: 'memory', group: '信息', blurb: '记住的事', load: () => import('./MemoryFeature.vue'), mini: () => import('./MemoryMini.vue') },
  { type: 'health', label: '健康', icon: 'heart_rate', group: '信息', blurb: '心率与提醒', load: () => import('./HealthFeature.vue'), mini: () => import('./HealthMini.vue') },
  { type: 'battery', label: '电量', icon: 'battery', group: '系统', blurb: '电量与充电', load: () => import('./BatteryFeature.vue'), mini: () => import('./BatteryMini.vue') },
  { type: 'network', label: '网络', icon: 'wifi', group: '系统', blurb: 'WiFi 与连接', load: () => import('./NetworkFeature.vue'), mini: () => import('./NetworkMini.vue') },
  { type: 'devices', label: '设备', icon: 'devices', group: '系统', blurb: '周边设备', load: () => import('./DevicesFeature.vue'), mini: () => import('./DevicesMini.vue') },
  { type: 'system', label: '系统', icon: 'system', group: '系统', blurb: '系统状态', load: () => import('./SystemFeature.vue'), mini: () => import('./SystemMini.vue') },
  { type: 'privacy', label: '隐私', icon: 'privacy', group: '系统', blurb: '摄像头与麦克风', load: () => import('./PrivacyFeature.vue'), mini: () => import('./PrivacyMini.vue') },
  { type: 'identity', label: '身份', icon: 'identity', group: '系统', blurb: '认主与家人', load: () => import('./IdentityFeature.vue'), mini: () => import('./IdentityMini.vue') },
  { type: 'home', label: '家居', icon: 'home', group: '系统', blurb: '家居联动', load: () => import('./HomeFeature.vue'), mini: () => import('./HomeMini.vue') },
  { type: 'sleep', label: '休眠', icon: 'moon', group: '系统', blurb: '休眠与唤醒', load: () => import('./SleepFeature.vue'), mini: () => import('./SleepMini.vue') },
  { type: 'pairing', label: '配对', icon: 'qr_code', group: '系统', blurb: '显示配对码', load: () => import('./PairingFeature.vue'), mini: () => import('./PairingMini.vue') },
];

export const FEATURE_GROUPS = ['媒体', '时间', '通讯', '信息', '系统'];
export const featureByType = (type: string): FeatureDef | undefined => FEATURES.find((f) => f.type === type);
