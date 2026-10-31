#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the real activity publisher with a controlled scene owner and clock."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('core', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ny-agent-eyes-') as directory:
    root = Path(directory)
    (root / 'nyabula_eye_service.h').write_text('''
struct nyabula_core_snapshot_s {
  struct { bool active; char source[32]; int priority; } scene_owner;
};
int nyabula_eye_service_snapshot(struct nyabula_core_snapshot_s *snapshot);
int nyabula_eye_service_submit(const char *source, const char *json, size_t length);
''')
    (root / 'test.c').write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NYABULA_CORE_EYE 1
#define __NYABULA_CORE_NY_AGENT_H
struct ny_agent_activity_s { char state[24]; unsigned int steps; uint64_t changed_ms; };
static struct ny_agent_activity_s activity;
static uint64_t clock_ms;
static char submitted[1024];
static int submissions;
static uint64_t ny_product_time_ms(bool monotonic) { assert(monotonic); return clock_ms; }
static void ny_agent_activity_get(struct ny_agent_activity_s *out) { *out = activity; }
#include "ny_agent_eyes.c"
static struct nyabula_core_snapshot_s scene;
int nyabula_eye_service_snapshot(struct nyabula_core_snapshot_s *out) { *out=scene; return 0; }
int nyabula_eye_service_submit(const char *source, const char *json, size_t length)
{
  assert(!strcmp(source,"nyagent-task")); assert(length < sizeof(submitted));
  assert(strstr(json,"\"id\":")==NULL);
  memcpy(submitted,json,length); submitted[length]=0; submissions++; return 0;
}
static void tick(const char *state)
{
  snprintf(activity.state,sizeof(activity.state),"%s",state);
  clock_ms+=500; assert(ny_agent_eyes_tick()==0);
}
int main(void)
{
  tick(""); assert(submissions==0);
  activity.steps=4; tick("queued");
  assert(strstr(submitted,"eyes.scene.show") && strstr(submitted,"queued"));
  assert(strstr(submitted,"Step 4") && strstr(submitted,"\"progress\":0"));
  scene.scene_owner.active=true; strcpy(scene.scene_owner.source,"alarm");
  scene.scene_owner.priority=60; int previous=submissions;
  tick("running"); assert(submissions==previous);
  scene.scene_owner.priority=25; tick("running"); assert(submissions==previous);
  scene.scene_owner.priority=10; tick("running"); assert(submissions==previous+1);
  strcpy(scene.scene_owner.source,"nyagent-task");
  tick("waiting_approval"); assert(strstr(submitted,"eyes.scene.update") && strstr(submitted,"confirm"));
  activity.changed_ms=clock_ms+500; tick("succeeded");
  assert(strstr(submitted,"done") && strstr(submitted,"\"progress\":1"));
  clock_ms+=2500; tick("succeeded"); assert(strstr(submitted,"core.release"));
  previous=submissions; tick("succeeded"); assert(submissions==previous);
  activity.changed_ms=clock_ms+500; tick("failed"); assert(strstr(submitted,"failed"));
  puts("AGENT_EYES_ACTIVITY_PRIORITY_UPDATE_RELEASE_PASS");
}
''')
    for flags in ([], ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I'+str(root), '-I'+str(a.core), str(root/'test.c'),
                        '-o', str(root/'test')], check=True)
        subprocess.run([str(root/'test')], check=True)
