/** Live task values have no invented demo totals or progress fractions. */
export function taskSceneData(payload: Record<string, unknown>, fallback: string) {
  const labels: Record<string, string> = {
    running: '执行中', queued: '排队中', confirm: '等待确认', done: '已完成', failed: '执行失败',
  };
  const requested = typeof payload.task_state === 'string' ? payload.task_state : fallback;
  const state = Object.prototype.hasOwnProperty.call(labels, requested) ? requested : 'running';
  const value = typeof payload.value === 'string' ? payload.value : '';
  const rawProgress = typeof payload.progress === 'number' && Number.isFinite(payload.progress) ? payload.progress : 0;
  const terminal = state === 'done' || state === 'failed';
  return {
    state,
    progress: value || rawProgress > 0 ? Math.max(0, Math.min(1, rawProgress)) : state === 'done' ? 1 : 0,
    indeterminate: !terminal && (payload.active === true || (!value && rawProgress <= 0)),
    value: value || (state === 'done' ? 'OK' : state === 'failed' ? '--' : '...'),
    title: typeof payload.title === 'string' && payload.title ? payload.title : labels[state],
    detail: typeof payload.detail === 'string' ? payload.detail : '',
  };
}
