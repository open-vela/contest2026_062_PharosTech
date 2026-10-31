import { describe, expect, it } from 'vitest';
import { taskSceneData } from '../src/taskScene.js';

describe('task scene live state', () => {
  it('uses actual values and preserves a real zero', () => {
    expect(taskSceneData({ value: '0%', progress: 0, title: '备份文件' }, 'queued'))
      .toMatchObject({ value: '0%', progress: 0, title: '备份文件', indeterminate: false });
    expect(taskSceneData({ value: '42%', progress: .42 }, 'running').progress).toBe(.42);
  });
  it('shows indeterminate activity instead of invented 3/5 counts', () => {
    expect(taskSceneData({}, 'running')).toMatchObject({ value: '...', progress: 0, indeterminate: true });
    expect(taskSceneData({ active: true, detail: 'Step 2' }, 'confirm'))
      .toMatchObject({ title: '等待确认', detail: 'Step 2', indeterminate: true });
  });
  it('keeps terminal results stable and follows payload task state', () => {
    expect(taskSceneData({ task_state: 'done' }, 'running')).toMatchObject({ value: 'OK', progress: 1, indeterminate: false });
    expect(taskSceneData({ task_state: 'failed' }, 'running')).toMatchObject({ value: '--', progress: 0, indeterminate: false });
  });
});
