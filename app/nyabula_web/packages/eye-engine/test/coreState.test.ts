import { describe, it, expect } from 'vitest';
import { coreEyeState, isCoreEyeSnapshot, type CoreEyeSnapshot } from '../src/coreState.js';
const snapshot: CoreEyeSnapshot = {
  schema:'nyabula.eye.v1', seq:12, uptime_ms:50000, expression:'curious', expression_since_ms:49000,
  scene:'sleep_timer', scene_style:'minimal', scene_since_ms:48000, scene_payload:{remaining_ms:4000, active:true},
  gaze_x:0.4, gaze_y:-0.5, gaze_until_ms:51200, gaze_active:true,
  ambient_light:0.55, auto_blink:false, iris_rgb:[0x38e06e,0xffb830], blink_nonce:3, blink_eyes:3,
};
describe('native Core projection', () => {
  it('maps monotonic ages without assuming device wall-clock synchronization', () => {
    const s=coreEyeState(snapshot,1700000000000);
    expect(s.expression?.since).toBe(1699999999000);
    expect(s.scene?.since).toBe(1699999998000);
    expect(s.gaze?.holdUntil).toBe(1700000001200);
    expect(s.appearance).toEqual({irisLeft:'#38e06e',irisRight:'#ffb830'});
    expect(s.scene?.type).toBe('sleep-timer');
    expect(s.autoBlink).toBe(false);
    expect(s.blinkNonce).toBe(3);
  });
  it('preserves authority for release and scene hide', () => {
    const s=coreEyeState({...snapshot,gaze_active:false,scene:'none'},1000);
    expect(s.gaze?.mode).toBe('auto'); expect(s.scene?.type).toBeNull();
  });
  it('rejects malformed or non-finite snapshots', () => {
    expect(isCoreEyeSnapshot(snapshot)).toBe(true);
    for(const bad of [null,{}, {...snapshot,gaze_x:NaN}, {...snapshot,iris_rgb:[]}, {...snapshot,scene_payload:null}]) {
      expect(isCoreEyeSnapshot(bad)).toBe(false);
    }
  });
});
