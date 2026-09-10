/* SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 */

/// <reference path="./nyabula.d.ts" />

export interface NyabulaPlugin {
  onStart?: () => void | Promise<void>;
  onEvent?: (event: string) => void | Promise<void>;
  onStop?: () => void | Promise<void>;
}

export function definePlugin<T extends NyabulaPlugin>(plugin: T): T {
  return plugin;
}

export * as core from "@nyabula/core";
export * as storage from "@nyabula/storage";
export * as network from "@nyabula/network";
export * as ui from "@nyabula/ui";
export * as ai from "@nyabula/ai";
