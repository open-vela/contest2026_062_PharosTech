#!/usr/bin/env bash
# tools/apply_dependency_patches.sh
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements. See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.
# The ASF licenses this file to you under the Apache License, Version 2.0
# (the "License"); you may not use this file except in compliance with
# the License. You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Apply product dependency patches without treating conflicts as success.
set -euo pipefail

workspace=$(cd "${1:?usage: apply_dependency_patches.sh WORKSPACE [PATCH_DIR]}" && pwd)
patch_dir=${2:-"$(cd "$(dirname "$0")/../patches" && pwd)"}

apply_one()
{
  local target=$1 patch_file=$2
  if patch --batch -d "$target" -p1 --forward --dry-run -s < "$patch_file" >/dev/null 2>&1; then
    patch --batch -d "$target" -p1 --forward -s < "$patch_file"
    echo "applied $(basename "$patch_file")"
  elif patch --batch -d "$target" -p1 --reverse --force --dry-run -s < "$patch_file" >/dev/null 2>&1; then
    echo "already applied $(basename "$patch_file")"
  else
    echo "dependency patch conflicts: $patch_file (target $target)" >&2
    return 1
  fi
}

shopt -s nullglob
app_patches=("$patch_dir"/apps-*.patch)
if (( ${#app_patches[@]} )); then
  applied=$(python3 "$(dirname "$0")/check_patch_stack.py" \
       "$workspace/apps" "${app_patches[@]}")
  for patch_file in "${app_patches[@]}"; do
    if grep -Fxq "$(basename "$patch_file")" <<< "$applied"; then
      echo "already applied $(basename "$patch_file")"
    else
      apply_one "$workspace/apps" "$patch_file"
    fi
  done
fi
agent_patches=("$patch_dir"/packages-ai_agent-*.patch)
if (( ${#agent_patches[@]} )); then
  applied=$(python3 "$(dirname "$0")/check_patch_stack.py" \
       "$workspace/packages/ai_agent" "${agent_patches[@]}")
  for patch_file in "${agent_patches[@]}"; do
    if grep -Fxq "$(basename "$patch_file")" <<< "$applied"; then
      echo "already applied $(basename "$patch_file")"
    else
      apply_one "$workspace/packages/ai_agent" "$patch_file"
    fi
  done
fi

# Native Bluetooth transport and persistent settings used by product_amp.
for patch_file in "$patch_dir"/external-zblue-*.patch; do
  [ -f "$patch_file" ] || continue
  apply_one "$workspace/apps/external/zblue" "$patch_file"
done
for patch_file in "$patch_dir"/zblue-*.patch; do
  [ -f "$patch_file" ] || continue
  apply_one "$workspace/apps/external/zblue/zblue" "$patch_file"
done
if [ -f "$patch_dir/nuttx-bluetooth-sco.patch" ]; then
  apply_one "$workspace/nuttx" "$patch_dir/nuttx-bluetooth-sco.patch"
fi
if [ -f "$patch_dir/nuttx-usb-audio-packet-boundary.patch" ] &&
   [ -f "$workspace/nuttx/drivers/usbhost/usbhost_audio.c" ]; then
  apply_one "$workspace/nuttx" "$patch_dir/nuttx-usb-audio-packet-boundary.patch"
fi
