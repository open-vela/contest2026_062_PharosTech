/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_agent_eyes.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#include "ny_agent.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>

#define NY_AGENT_EYE_SOURCE    "nyagent-task"
#define NY_AGENT_EYE_PRIORITY  25
#define NY_AGENT_EYE_LEASE_MS  1500
#define NY_AGENT_EYE_RESULT_MS 2500

static uint64_t g_agent_eye_next;
static bool g_agent_eye_published;
#endif

/****************************************************************************
 * Name: ny_agent_eyes_tick
 ****************************************************************************/

int ny_agent_eyes_tick(void)
{
#ifdef CONFIG_NYABULA_CORE_EYE
  uint64_t now = ny_product_time_ms(true);
  if (now < g_agent_eye_next)
    return 0;
  g_agent_eye_next = now + 500;
  struct ny_agent_activity_s activity;
  ny_agent_activity_get(&activity);
  bool queued = !strcmp(activity.state, "queued");
  bool running = !strcmp(activity.state, "running");
  bool confirm = !strcmp(activity.state, "waiting_approval");
  bool cancelling = !strcmp(activity.state, "cancelling");
  bool done = !strcmp(activity.state, "succeeded");
  bool failed = !strcmp(activity.state, "failed") ||
                !strcmp(activity.state, "cancelled") ||
                !strcmp(activity.state, "unknown");
  bool active = queued || running || confirm || cancelling;
  bool show = active || ((done || failed) &&
                         now < activity.changed_ms + NY_AGENT_EYE_RESULT_MS);
  if (!show && !g_agent_eye_published)
    return 0;
  char command[640];
  int length;

  /* Unacknowledged refreshes must not replace a synchronous caller's
   * last_request_id confirmation on the display owner thread.
   */

  if (!show)
    length = snprintf(command, sizeof(command),
                      "{\"source\":\"" NY_AGENT_EYE_SOURCE
                      "\",\"action\":\"core.release\",\"params\":{}}");
  else
    {
      struct nyabula_core_snapshot_s snapshot;
      int ret = nyabula_eye_service_snapshot(&snapshot);
      if (ret < 0)
        return ret == -ENODEV ? 0 : ret;
      bool owned = snapshot.scene_owner.active &&
                   !strcmp(snapshot.scene_owner.source, NY_AGENT_EYE_SOURCE);
      if (!owned && snapshot.scene_owner.active &&
          snapshot.scene_owner.priority >= NY_AGENT_EYE_PRIORITY)
        return 0;
      const char *state = confirm || cancelling ? "confirm"
                          : queued              ? "queued"
                          : done                ? "done"
                          : failed              ? "failed"
                                                : "running";
      const char *title = confirm      ? "等待确认"
                          : cancelling ? "取消中"
                          : queued     ? "排队中"
                          : done       ? "已完成"
                          : !strcmp(activity.state, "cancelled") ? "已取消"
                          : failed ? "执行失败"
                                   : "执行中";
      length = snprintf(
          command, sizeof(command),
          "{\"source\":\"" NY_AGENT_EYE_SOURCE
          "\",\"action\":\"%s\",\"priority\":%d,\"lease_ms\":%d,"
          "\"params\":{%s\"payload\":{"
          "\"task_state\":\"%s\",\"title\":\"%s\",\"detail\":\"Step %u\","
          "\"value\":\"%s\",\"progress\":%d,\"active\":%s}}}",
          owned ? "eyes.scene.update" : "eyes.scene.show",
          NY_AGENT_EYE_PRIORITY, NY_AGENT_EYE_LEASE_MS,
          owned ? "" : "\"scene\":\"task\",\"style\":\"full\",", state, title,
          activity.steps,
          done     ? "OK"
          : failed ? "--"
                   : "...",
          done ? 1 : 0, active ? "true" : "false");
    }
  if (length < 0 || length >= (int)sizeof(command))
    return -E2BIG;
  int ret = nyabula_eye_service_submit(NY_AGENT_EYE_SOURCE, command, length);
  if (ret == 0)
    g_agent_eye_published = show;
  return ret == -ENODEV || ret == -EAGAIN ? 0 : ret;
#else
  return 0;
#endif
}
