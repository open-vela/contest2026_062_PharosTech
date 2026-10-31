import * as ui from '@nyabula/ui';
import { request, type Topic } from '@nyabula/hardware';

let message = '选择一项检查。硬件权限默认关闭，可在权限页按需开启。';
const actions: { label: string; topic: Topic; icon: string }[] = [
  { label: '声卡状态', topic: 'audio.status', icon: 'volume_up' },
  { label: 'USB 音频设备', topic: 'usb.devices', icon: 'usb' },
  { label: '蓝牙状态', topic: 'bt.status', icon: 'bluetooth' },
  { label: 'DSP 状态', topic: 'dsp.status', icon: 'equalizer' },
];
function render() {
  ui.render({ nyaui: 1, page: { title: '硬件小助手', icon: 'settings' }, root: {
    type: 'column', props: { gap: 16, padding: 20 }, children: [
      { type: 'text', props: { text: '设备能力，一眼看清' } },
      { type: 'text', id: 'result', props: { text: message } },
      ...actions.map((item, i) => ({ type: 'button', id: `check-${i}`, props: { text: item.label, icon: item.icon }, on: { tap: { command: item.topic } } })),
    ],
  } });
}
export default {
  onStart() { render(); },
  async onCommand(command: string) {
    if (!actions.some(item => item.topic === command)) throw Error('Unknown command');
    try {
      const data = await request(command as Topic);
      message = JSON.stringify(data, null, 2);
    } catch (error) {
      message = `暂时无法读取：${String(error)}。请检查此插件的权限与硬件状态。`;
    }
    render();
  },
};
