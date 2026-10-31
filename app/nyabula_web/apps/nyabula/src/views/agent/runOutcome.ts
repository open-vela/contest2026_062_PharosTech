import type { NyabotRun } from '../../stores/nyabot';

/* Device records, never conclusions inferred from the model's prose. */
export function runOutcome(run: NyabotRun): string[] {
  if (run.state === 'unknown') return ['执行结果未知，请核对后再决定是否重试'];
  const steps = run.steps ?? [];
  if (!steps.length) {
    return run.state === 'succeeded' ? ['仅文字回复，未调用工具'] : [];
  }
  return steps.flatMap(step => {
    const name = step.topic === 'agent.tools.call' && typeof step.arguments?.name === 'string'
      ? step.arguments.name : step.topic;
    let result: string;
    if (step.sideEffectUncertain) result = '结果待核对，请勿重试';
    else if (step.sideEffectApplied) result = step.error
      ? `已执行，但返回结果失败（错误 ${step.error}）` : '已执行成功';
    else if (step.decision === 'deny') result = '已拒绝，未执行';
    else if (step.state === 'succeeded') result = '执行成功';
    else if (step.state === 'recovered') result = '已由后续步骤修复';
    else if (step.state === 'failed') result = `未成功${step.error ? `（错误 ${step.error}）` : ''}`;
    else if (step.state === 'authorized') result = '已允许，等待执行结果';
    else return [];
    return [`${name} · ${result}`];
  });
}
