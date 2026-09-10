/* SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 */

declare module "@nyabula/core" {
  export interface PluginInfo {
    id: string;
    version: string;
    apiVersion: number;
  }

  export function info(): PluginInfo;
  export function log(...values: unknown[]): void;
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
  export function notify(message: string): Promise<void>;
}

declare module "@nyabula/ai" {
  export function invoke(prompt: string): Promise<string>;
}
