import { expect, it } from 'vitest';
import { runOutcome } from '../src/views/agent/runOutcome';
import type { NyabotRun } from '../src/stores/nyabot';

const run: NyabotRun = { id: 'test', requestId: 'test', conversationId: 'test',
  text: 'write a test file', reply: 'Waiting for approval', state: 'succeeded', createdAt: 1 };
const step = { tool: 'nyabula_action', topic: 'agent.tools.call', arguments: { name: 'write_file' }, state: 'succeeded' };
it('does not mistake a text promise for a submitted operation', () => {
  expect(runOutcome(run)).toEqual(['仅文字回复，未调用工具']);
  expect(runOutcome({ ...run, state: 'running' })).toEqual([]);
});
it('shows the recorded write despite stale model prose', () => {
  expect(runOutcome({ ...run, steps: [{ ...step, decision: 'allow', sideEffectApplied: true }] }))
    .toEqual(['write_file · 已执行成功']);
});
it('distinguishes a denied decision from a missing permission', () => {
  expect(runOutcome({ ...run, state: 'failed', steps: [{ ...step, state: 'failed', decision: 'deny', error: -13, sideEffectApplied: false }] }))
    .toEqual(['write_file · 已拒绝，未执行']);
});
it('does not hide an applied or uncertain effect behind a failed response', () => {
  expect(runOutcome({ ...run, steps: [{ ...step, state: 'failed', error: -7, sideEffectApplied: true }] }))
    .toEqual(['write_file · 已执行，但返回结果失败（错误 -7）']);
  expect(runOutcome({ ...run, steps: [{ ...step, sideEffectUncertain: true }] }))
    .toEqual(['write_file · 结果待核对，请勿重试']);
  expect(runOutcome({ ...run, state: 'unknown' })).toEqual(['执行结果未知，请核对后再决定是否重试']);
});
it('keeps authorization distinct from execution', () => {
  expect(runOutcome({ ...run, state: 'waiting_approval', steps: [{ ...step, state: 'pending' }] })).toEqual([]);
  expect(runOutcome({ ...run, steps: [{ ...step, state: 'authorized' }] })).toEqual(['write_file · 已允许，等待执行结果']);
});
