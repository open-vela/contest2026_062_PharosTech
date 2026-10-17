/* Protocol-facing types for eye.state (NyaLink v1, Shared/protocol/nyalink.md). */

export interface EyeStateExpression {
  mode: string;
  since: number; // epoch ms when the mode was entered
  lightLevel: number; // 0..1
  sleepLid?: [number, number]; // only mode=sleep: [lidTop, lidBot] snapshot
}

export interface EyeStateGaze {
  mode: 'auto' | 'target';
  x: number;
  y: number;
  holdUntil: number; // epoch ms
}

export interface EyeStateScene {
  type: string | null;
  style?: 'full' | 'minimal';
  since?: number; // epoch ms
  options?: Record<string, unknown>;
  payload?: Record<string, unknown>;
}

export interface EyeOneShot {
  type: string; // e.g. "blink"
  at: number; // epoch ms
}

export interface EyeState {
  autoBlink?: boolean;
  blinkNonce?: number;
  blinkEyes?: number;
  seq?: number;
  t?: number; // sender epoch ms
  expression?: EyeStateExpression;
  gaze?: EyeStateGaze;
  appearance?: { irisLeft?: string; irisRight?: string };
  scene?: EyeStateScene;
  oneShot?: EyeOneShot[];
}
