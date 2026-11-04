export interface PrivacyState { camera: boolean; mic: boolean; revision: number }
export function isPrivacyState(value: unknown): value is PrivacyState {
  const v = value as PrivacyState | null;
  return !!v && typeof v.camera === 'boolean' && typeof v.mic === 'boolean' &&
    Number.isSafeInteger(v.revision) && v.revision >= 0;
}
