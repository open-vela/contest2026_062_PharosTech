/*
 * Eye parameter definitions loaded from the single source of truth
 * packages/eye-params/eye-params.json (imported at runtime, never copied).
 */
import rawParams from '@nyabula/eye-params/eye-params.json';

export interface ParamSpec {
  default: number;
  min: number;
  max: number;
  speed: number;
}

export interface EyeParamsJson {
  version: number;
  params: Record<string, ParamSpec>;
  modes: string[];
  scenes: string[];
  sceneStyles: string[];
  timing: {
    blinkSpeed: number;
    blinkClosePortion: number;
    blinkIntervalMin: number;
    blinkIntervalRand: number;
    doubleBlinkChance: number;
    doubleBlinkDelay: number;
    saccadeIntervalMin: number;
    saccadeIntervalRand: number;
    saccadeAmpX: number;
    saccadeAmpY: number;
    saccadeRecenterChance: number;
    lookHoldDefault: number;
    sceneCloseTime: number;
    sceneOpenTime: number;
    sceneFadeTime: number;
    sceneEase: [number, number];
    sleepCloseDuration: number;
    /** Per second: how fast the light the pupil reacts to follows the
     *  reported level while it rises (pupil narrows) and falls (widens). */
    lightConstrictSpeed: number;
    lightDilateSpeed: number;
  };
  appearance: { irisDefault: string; pupilBase: string };
  /** Device panel geometry in panel pixels (nyabula_eye_renderer_lvgl.c). */
  geometry: { panelSizePx: number; eyeRadiusPx: number; globeInsetPx: number };
}

export const EYE_PARAMS = rawParams as unknown as EyeParamsJson;

export const PARAM_KEYS = Object.keys(EYE_PARAMS.params);

/** Interpolation speed table, sourced from eye-params.json. */
export const SPEED: Record<string, number> = Object.fromEntries(
  PARAM_KEYS.map((k) => [k, EYE_PARAMS.params[k].speed]),
);

export type EyeParams = Record<string, number>;

/** Fresh parameter object using the JSON defaults. Plain object on purpose:
 *  cur/tgt must never be wrapped by a reactivity system. */
export function mkParams(): EyeParams {
  const p: EyeParams = {};
  for (const k of PARAM_KEYS) p[k] = EYE_PARAMS.params[k].default;
  return p;
}

export type EyeMode = string;
export type SceneType = string;
export type SceneStyle = 'full' | 'minimal';

export const MODES = EYE_PARAMS.modes;
export const SCENES = EYE_PARAMS.scenes;
export const TIMING = EYE_PARAMS.timing;
export const IRIS_DEFAULT = EYE_PARAMS.appearance.irisDefault;
