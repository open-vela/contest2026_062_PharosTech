import { describe, expect, it } from 'vitest';
import { FEATURES } from '../src/views/services/features';
import { isPrivacyState } from '../src/lib/privacy';
import { readFileSync } from 'node:fs';

describe('feature finishing', () => {
 it('hides battery, home and cancelled health for this batch', () => {
  expect(FEATURES.some(f => ['battery','home','health'].includes(f.type))).toBe(false);
 });
 it('ties presence to the real identity service', () => {
  const presence = FEATURES.find(f => f.type === 'presence')!;
  expect(presence.blurb).toContain('身份'); expect(presence.stage).toBe('ready');
 });
 it('offers real privacy controls, not local display preferences', () => {
  expect(FEATURES.find(f => f.type === 'privacy')?.stage).toBe('ready');
  expect(isPrivacyState({ camera:true, mic:false, revision:0 })).toBe(true);
  for (const value of [null, {}, {camera:1,mic:true,revision:0}, {camera:true,mic:true,revision:-1}]) expect(isPrivacyState(value)).toBe(false);
 });
 it('places owner voiceprint enrollment inside identity', () => {
  const source=readFileSync('src/views/services/features/IdentityFeature.vue','utf8');
  expect(source).toContain("import SpeakerControls from './SpeakerControls.vue'");
  expect(source).toContain('<SpeakerControls />');
 });
 it('uses the shared styled button for settings', () => {
  const source = readFileSync('src/views/services/FeatureCard.vue', 'utf8');
  expect(source).toContain('variant="tonal" class="fc-settings"');
 });
});
