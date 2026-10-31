/* SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 */

declare module "@nyabula/core" {
  export interface PluginInfo {
    id: string;
    version: string;
    apiVersion: number;
    permissions: string[];
  }

  export function info(): PluginInfo;
  export function log(...values: unknown[]): void;
}

declare module "@nyabula/hardware" {
  /** Exact capability-gated product topics; no filesystem or arbitrary RPC. */
  export type Topic =
    | "audio.status" | "audio.volume" | "audio.channel" | "audio.output.route"
    | "audio.input.route" | "audio.mic.gain" | "audio.mic.mute" | "audio.capture"
    | "music.library" | "music.status" | "music.play" | "music.pause" | "music.resume" | "music.stop"
    | "dsp.status" | "dsp.config" | "dsp.play" | "dsp.stop"
    | "bt.status" | "bt.devices" | "bt.discoverable" | "bt.pair.confirm"
    | "bt.connect" | "bt.disconnect" | "bt.forget"
    | "bt.media.play" | "bt.media.pause" | "bt.media.stop" | "bt.media.next" | "bt.media.prev"
    | "bt.call.answer" | "bt.call.reject" | "bt.call.hangup"
    | "usb.devices" | "camera.capture"
    | "weather.status" | "weather.get" | "weather.query"
    | "sys.info" | "device.status"
    | "nas.status" | "nas.devices" | "nas.list" | "nas.configure" | "nas.mkdir";
  export function request<T = Record<string, unknown>>(topic: Topic, data?: Record<string, unknown>): Promise<T>;
}

declare module "@nyabula/storage" {
  export function get(key: string): string | null;
  export function put(key: string, value: string): void;
}

declare module "@nyabula/network" {
  export interface HttpResponse {
    status: number;
    body: string;
  }
  export function request(url: string): Promise<HttpResponse>;
  /** Legacy mock-only echo signature; not supported by the real HTTP provider. */
  export function request(request: unknown): Promise<unknown>;
}

declare module "@nyabula/ui" {
  export interface EyeCommand {
    action: "eyes.expression" | "eyes.blink" | "eyes.gaze" | "eyes.iris"
      | "eyes.auto_blink" | "eyes.ambient" | "eyes.scene.show"
      | "eyes.scene.update" | "eyes.scene.hide" | "core.release";
    params: Record<string, unknown>;
    id?: string;
    priority?: number;
    lease_ms?: number;
  }
  /** Resolves on queue admission, not on panel presentation. */
  export function eye(command: EyeCommand): Promise<void>;
  /** Eye caption: at most 47 UTF-8 bytes. Put longer feedback in the page. */
  export function notify(message: string): Promise<void>;
  export interface NyaUiDocument {
    nyaui: 1;
    views?: { component?: NyaUiDocument; html?: NyaUiDocument };
    page?: { title?: string; icon?: string; placement?: string };
    root?: { type: string; id?: string; props?: Record<string, unknown>;
      children?: unknown[]; on?: Record<string, unknown> };
    html?: string;
    events?: Record<string, { command: string; args?: Record<string, unknown> }>;
  }
  /** Publish a page. Requires ui.page, or ui.html for isolated HTML. */
  export function render(document: NyaUiDocument): void;
}

declare module "@nyabula/ai" {
  export function invoke(prompt: string): Promise<string>;
}
