// SPDX-License-Identifier: Apache-2.0
// Generated and iterated by the on-device Nyabula Agent (MiMo 2.6 Flash).
import * as storage from '@nyabula/storage';
import * as ui from '@nyabula/ui';
import * as core from '@nyabula/core';

const KEY = 'transfer.history';
const LIMIT = 5;
const STORE = 4096;
const NMAX = 47;

let state = { size: '', speed: '', eff: '80', su: 'GB', vu: 'Mbps', errs: [], res: null };

function bytesLen(s) { return unescape(encodeURIComponent(s)).length; }
function notify(s) { const t = String(s == null ? '' : s); ui.notify(bytesLen(t) > NMAX ? t.slice(0, 15) : t); }
function loadHistory() {
  try { const raw = storage.get(KEY); if (!raw) return []; const list = JSON.parse(raw); return Array.isArray(list) ? list.slice(0, LIMIT) : []; }
  catch (e) { return []; }
}
function saveHistory(list) {
  const kept = list.slice(0, LIMIT);
  const raw = JSON.stringify(kept);
  if (bytesLen(raw) > STORE) throw new Error('历史记录超出存储上限');
  storage.put(KEY, raw);
}
function toNum(v) { if (typeof v === 'number') return v; if (v == null) return NaN; const s = String(v).trim(); return s === '' ? NaN : Number(s); }
function trimNum(n) { return isFinite(n) ? String(Math.round(n * 1000000) / 1000000) : String(n); }
function fmtDur(sec) {
  const t = Math.max(0, Math.floor(sec)), h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), s = t % 60;
  return h > 0 ? h + '小时' + m + '分' + s + '秒' : m > 0 ? m + '分' + s + '秒' : s + '秒';
}
function compute(st) {
  const size = toNum(st.size), speed = toNum(st.speed), eff = toNum(st.eff);
  if (!isFinite(size) || size <= 0) st.errs.push('请输入大于0的容量。');
  if (!isFinite(speed) || speed <= 0) st.errs.push('请输入大于0的速度。');
  if (!isFinite(eff) || eff <= 0 || eff > 100) st.errs.push('效率须在1到100之间。');
  if (st.errs.length) return null;
  const sb = st.su === 'GiB' ? size * Math.pow(2, 30) : size * 1e9;
  const bps = st.vu === 'MB/s' ? speed * 1e6 : speed * 1e6 / 8;
  const sec = sb / (bps * eff / 100);
  if (!isFinite(sec) || sec <= 0) { st.errs.push('结果超出可用范围。'); return null; }
  return { size, su: st.su, speed, vu: st.vu, eff, sec };
}
function histChildren() {
  const list = loadHistory();
  if (!list.length) return [{ type: 'text', id: 'he', props: { text: '暂无历史' } }];
  return list.map((it, i) => ({ type: 'text', id: 'h' + i, props: { text: trimNum(it.size) + ' ' + it.su + '，' + trimNum(it.speed) + ' ' + it.vu + '，效率' + trimNum(it.eff) + '%→' + trimNum(it.sec) + '秒（' + fmtDur(it.sec) + '）' } }));
}
function view() {
  const res = [];
  if (state.errs.length) state.errs.forEach((t, i) => res.push({ type: 'text', id: 'e' + i, props: { text: t } }));
  else if (state.res) {
    const r = state.res;
    res.push({ type: 'statCard', id: 'rs', props: { label: '预计秒数', value: trimNum(r.sec), unit: '秒', icon: 'schedule' } });
    res.push({ type: 'statCard', id: 'rh', props: { label: '预计时长', value: fmtDur(r.sec), unit: '', icon: 'timer' } });
    res.push({ type: 'text', id: 'rn', props: { text: '按' + trimNum(r.size) + ' ' + r.su + '、' + trimNum(r.speed) + ' ' + r.vu + '、效率' + trimNum(r.eff) + '%估算，非实测。' } });
  } else res.push({ type: 'text', id: 're', props: { text: '填写容量、速度、效率后点开始计算。' } });
  const hist = histChildren();
  return { nyaui: 1, page: { title: '传输时间计算器', icon: 'schedule', placement: 'menu' }, root: { type: 'column', id: 'root', props: {}, children: [
    { type: 'section', id: 'form', props: {}, children: [
      { type: 'text', id: 'ft', props: { text: '计算参数' } },
      { type: 'textField', id: 'size', props: { label: '容量数值', value: state.size, placeholder: '例如8' } },
      { type: 'row', id: 'su', props: {}, children: [
        { type: 'button', id: 'ugb', props: { text: 'GB(10^9字节)', variant: state.su === 'GB' ? 'filled' : 'default' }, on: { tap: { command: 'set-su', args: { su: 'GB' } } } },
        { type: 'button', id: 'ugib', props: { text: 'GiB(2^30字节)', variant: state.su === 'GiB' ? 'filled' : 'default' }, on: { tap: { command: 'set-su', args: { su: 'GiB' } } } }
      ] },
      { type: 'text', id: 'sun', props: { text: '容量单位：' + state.su } },
      { type: 'textField', id: 'speed', props: { label: '速度数值', value: state.speed, placeholder: '例如100' } },
      { type: 'row', id: 'vu', props: {}, children: [
        { type: 'button', id: 'umbps', props: { text: 'Mbps(10^6比特/秒)', variant: state.vu === 'Mbps' ? 'filled' : 'default' }, on: { tap: { command: 'set-vu', args: { vu: 'Mbps' } } } },
        { type: 'button', id: 'umbs', props: { text: 'MB/s(10^6字节/秒)', variant: state.vu === 'MB/s' ? 'filled' : 'default' }, on: { tap: { command: 'set-vu', args: { vu: 'MB/s' } } } }
      ] },
      { type: 'text', id: 'vun', props: { text: '速度单位：' + state.vu } },
      { type: 'textField', id: 'efficiency', props: { label: '效率(1-100)', value: state.eff, placeholder: '例如80' } }
    ] },
    { type: 'row', id: 'act', props: {}, children: [
      { type: 'button', id: 'calc', props: { text: '开始计算', variant: 'filled' }, on: { tap: { command: 'save' } } },
      { type: 'button', id: 'clr', props: { text: '清空我的历史', variant: 'default' }, on: { tap: { command: 'clear' } } }
    ] },
    { type: 'card', id: 'res', props: {}, title: '估算结果', children: res },
    { type: 'card', id: 'hist', props: {}, title: '计算历史', children: [{ type: 'text', id: 'ht', props: { text: '最近' + LIMIT + '次' } }].concat(hist) }
  ] } };
}
function render() { ui.render(view()); }

export default {
  onStart() { state = { size: '', speed: '', eff: '80', su: 'GB', vu: 'Mbps', errs: [], res: null }; render(); },
  onCommand(command, args, values) {
    if (command === 'clear') {
      try { saveHistory([]); notify('已清空历史'); }
      catch (e) { core.log('clear failed: ' + e); state.errs = ['清空历史失败，请稍后重试。']; state.res = null; render(); return; }
      state = { size: state.size, speed: state.speed, eff: state.eff, su: state.su, vu: state.vu, errs: [], res: null }; render(); return;
    }
    if (command === 'set-su' || command === 'set-vu') {
      if (command === 'set-su' && args && (args.su === 'GB' || args.su === 'GiB')) state.su = args.su;
      if (command === 'set-vu' && args && (args.vu === 'Mbps' || args.vu === 'MB/s')) state.vu = args.vu;
      const v = values || {};
      if (v.size != null) state.size = String(v.size);
      if (v.speed != null) state.speed = String(v.speed);
      if (v.efficiency != null) state.eff = String(v.efficiency);
      state.errs = []; state.res = null; render(); return;
    }
    if (command !== 'save') return;
    const v = values || {};
    state.size = v.size == null ? '' : String(v.size);
    state.speed = v.speed == null ? '' : String(v.speed);
    state.eff = v.efficiency == null ? '' : String(v.efficiency);
    state.errs = []; state.res = null;
    const r = compute(state);
    if (!r) { notify('请检查输入'); render(); return; }
    try { saveHistory([r].concat(loadHistory())); }
    catch (e) { core.log('save failed: ' + e); state.errs = ['结果计算成功，但保存历史失败。']; render(); return; }
    state.res = r; notify('估算完成'); render();
  },
  onStop() {}
};