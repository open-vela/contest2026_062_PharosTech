/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/nycore_main.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ny_health.h"
#include "ny_package.h"
#include "ny_permission.h"
#include "ny_revocation.h"
#include "ny_runtime.h"
#include "ny_scheduler.h"
#include "ny_wasm.h"
#ifdef CONFIG_NYABULA_CORE_EYE
#include <nyabula_eye_service.h>
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void nycore_usage(void);
static int nycore_run(const char *path, const char *event);
static int nycore_run_package(const char *path, const char *event);
static int nycore_start_installed(const char *id);
static int nycore_isolation(void);
#ifdef CONFIG_NYABULA_CORE_EYE
static int nycore_eye(const char *path);
static int nycore_eye_status(void);
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void nycore_usage(void)
{
  fprintf(stderr, "Usage:\n"
                  "  nycore run <plugin.js> [-e <event>]\n"
                  "  nycore run-package <package-dir> [-e <event>]\n"
                  "  nycore run-wasm <module.wasm>\n"
                  "  nycore start <id> <plugin.js>\n"
                  "  nycore start-package <package-dir>\n"
                  "  nycore event <id> <event>\n"
                  "  nycore stop <id>\n"
                  "  nycore stop-all\n"
                  "  nycore grant <id> <permission>\n"
                  "  nycore revoke <id> <permission>\n"
                  "  nycore permissions <id>\n"
                  "  nycore install <package-dir>\n"
                  "  nycore activate <id> <version>\n"
                  "  nycore promote <id> <version>\n"
                  "  nycore rollback <id>\n"
                  "  nycore start-installed <id>\n"
                  "  nycore health <id> <version>\n"
                  "  nycore unquarantine <id> <version>\n"
                  "  nycore revoke-key <key-id>\n"
                  "  nycore allow-key <key-id>\n"
                  "  nycore revoke-package <id> <version>\n"
                  "  nycore allow-package <id> <version>\n"
                  "  nycore revocations\n"
                  "  nycore packages\n"
                  "  nycore isolation\n"
                  "  nycore list\n");
#ifdef CONFIG_NYABULA_CORE_EYE
  fprintf(stderr, "  nycore eye <command.json>\n"
                  "  nycore eye-status\n");
#endif
}

#ifdef CONFIG_NYABULA_CORE_EYE
static int nycore_eye(const char *path)
{
  FILE *stream;
  char *json = malloc(NYABULA_EYE_JSON_LIMIT + 1);
  size_t length;
  int ret;
  if (json == NULL)
    {
      return -ENOMEM;
    }
  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      ret = -errno;
      free(json);
      return ret;
    }
  length = fread(json, 1, NYABULA_EYE_JSON_LIMIT + 1, stream);
  ret =
      ferror(stream) ? -EIO : nyabula_eye_service_submit("nsh", json, length);
  fclose(stream);
  free(json);
  return ret;
}

static int nycore_eye_status(void)
{
  struct nyabula_core_snapshot_s state;
  int ret = nyabula_eye_service_snapshot(&state);
  if (ret == 0)
    {
      printf("eye revision=%" PRIu64 " expression=%d scene=%d queued=%zu "
             "status=%d source=%s\n",
             state.revision, state.expression, state.scene, state.queue_depth,
             state.last_status, state.expression_owner.source);
    }
  return ret;
}
#endif

static int nycore_isolation(void)
{
  int local;

#ifdef __aarch64__
#ifdef CONFIG_BUILD_KERNEL
  printf("build=kernel privilege=user stack=%p\n", &local);
#else
  unsigned long current_el;

  __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
  printf("build=flat current_el=%lu stack=%p\n", (current_el >> 2) & 3,
         &local);
#endif
#else
  printf("build=%s current_el=host stack=%p\n",
#ifdef CONFIG_BUILD_KERNEL
         "kernel",
#else
         "flat",
#endif
         &local);
#endif
  return 0;
}

static int nycore_run(const char *path, const char *event)
{
  struct ny_plugin_s plugin;
  int ret;

  ret = ny_plugin_load(&plugin, path);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: load failed: %d\n", ret);
      ny_plugin_destroy(&plugin);
      return ret;
    }

  ret = ny_plugin_start(&plugin);
  if (ret >= 0 && event != NULL)
    {
      ret = ny_plugin_dispatch(&plugin, event);
    }

  if (ret >= 0)
    {
      ret = ny_plugin_stop(&plugin);
    }

  ny_plugin_destroy(&plugin);
  return ret;
}

static int nycore_run_package(const char *path, const char *event)
{
  struct ny_plugin_config_s config;
  struct ny_plugin_s plugin;
  int ret;

  ret = ny_manifest_load(path, &config);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: manifest rejected: %d\n", ret);
      return ret;
    }

  if (config.runtime == NY_PLUGIN_RUNTIME_WAMR)
    {
      return event == NULL ? ny_wasm_run_config(&config) : -ENOTSUP;
    }

  ret = ny_plugin_load_config(&plugin, &config);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: package load failed: %d\n", ret);
      ny_plugin_destroy(&plugin);
      return ret;
    }

  ret = ny_plugin_start(&plugin);
  if (ret >= 0 && event != NULL)
    {
      ret = ny_plugin_dispatch(&plugin, event);
    }

  if (ret >= 0)
    {
      ret = ny_plugin_stop(&plugin);
    }

  ny_plugin_destroy(&plugin);
  return ret;
}

static int nycore_start_installed(const char *id)
{
  char path[PATH_MAX];
  char storage_root[PATH_MAX];
  int ret;

  ret = ny_package_storage_root(id, storage_root, sizeof(storage_root));
  if (ret >= 0)
    {
      ret = ny_package_resolve(id, path, sizeof(path));
    }

  if (ret >= 0)
    {
      ret = ny_scheduler_start_installed(path, storage_root);
    }

  if (ret != -EACCES)
    {
      return ret;
    }

  fprintf(stderr, "nycore: current version rejected, rolling back\n");
  ret = ny_package_rollback(id);
  if (ret >= 0)
    {
      ret = ny_package_resolve(id, path, sizeof(path));
    }

  return ret < 0 ? ret : ny_scheduler_start_installed(path, storage_root);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2)
    {
      nycore_usage();
      return EXIT_FAILURE;
    }

#ifdef CONFIG_NYABULA_CORE_EYE
  if (strcmp(argv[1], "eye-status") == 0 && argc == 2)
    {
      ret = nycore_eye_status();
    }
  else if (strcmp(argv[1], "eye") == 0 && argc == 3)
    {
      ret = nycore_eye(argv[2]);
    }
  else
#endif
      if (strcmp(argv[1], "run") == 0)
    {
      const char *event = NULL;

      if (argc != 3 && argc != 5)
        {
          nycore_usage();
          return EXIT_FAILURE;
        }

      if (argc == 5)
        {
          if (strcmp(argv[3], "-e") != 0)
            {
              nycore_usage();
              return EXIT_FAILURE;
            }

          event = argv[4];
        }

      ret = nycore_run(argv[2], event);
    }
  else if (strcmp(argv[1], "run-wasm") == 0 && argc == 3)
    {
      ret = ny_wasm_run(argv[2], "local.wasm", NY_PERMISSION_CORE_LOG);
    }
  else if (strcmp(argv[1], "run-package") == 0)
    {
      const char *event = NULL;

      if (argc != 3 && argc != 5)
        {
          nycore_usage();
          return EXIT_FAILURE;
        }

      if (argc == 5)
        {
          if (strcmp(argv[3], "-e") != 0)
            {
              nycore_usage();
              return EXIT_FAILURE;
            }

          event = argv[4];
        }

      ret = nycore_run_package(argv[2], event);
    }
  else if (strcmp(argv[1], "start") == 0 && argc == 4)
    {
      ret = ny_scheduler_start(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "start-package") == 0 && argc == 3)
    {
      ret = ny_scheduler_start_package(argv[2]);
    }
  else if (strcmp(argv[1], "event") == 0 && argc == 4)
    {
      ret = ny_scheduler_dispatch(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "stop") == 0 && argc == 3)
    {
      ret = ny_scheduler_stop(argv[2]);
    }
  else if (strcmp(argv[1], "stop-all") == 0 && argc == 2)
    {
      ret = ny_scheduler_stop_all();
    }
  else if (strcmp(argv[1], "grant") == 0 && argc == 4)
    {
      ret = ny_permission_grant(argv[2], argv[3]);
      if (ret >= 0)
        {
          ret = ny_scheduler_refresh_permissions(argv[2]);
        }
    }
  else if (strcmp(argv[1], "revoke") == 0 && argc == 4)
    {
      ret = ny_permission_revoke(argv[2], argv[3]);
      if (ret >= 0)
        {
          ret = ny_scheduler_refresh_permissions(argv[2]);
        }
    }
  else if (strcmp(argv[1], "permissions") == 0 && argc == 3)
    {
      ret = ny_permission_show(argv[2]);
    }
  else if (strcmp(argv[1], "install") == 0 && argc == 3)
    {
      ret = ny_package_install(argv[2]);
    }
  else if (strcmp(argv[1], "activate") == 0 && argc == 4)
    {
      ret = ny_package_activate(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "promote") == 0 && argc == 4)
    {
      char path[PATH_MAX];

      ret = ny_package_resolve_version(argv[2], argv[3], path, sizeof(path));
      if (ret >= 0)
        {
          ret = nycore_run_package(path, NULL);
        }

      if (ret >= 0)
        {
          ret = ny_package_activate(argv[2], argv[3]);
        }
    }
  else if (strcmp(argv[1], "rollback") == 0 && argc == 3)
    {
      ret = ny_package_rollback(argv[2]);
    }
  else if (strcmp(argv[1], "start-installed") == 0 && argc == 3)
    {
      ret = nycore_start_installed(argv[2]);
    }
  else if (strcmp(argv[1], "health") == 0 && argc == 4)
    {
      ret = ny_health_show(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "unquarantine") == 0 && argc == 4)
    {
      ret = ny_health_reset(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "revoke-key") == 0 && argc == 3)
    {
      ret = ny_revocation_update_key(argv[2], true);
    }
  else if (strcmp(argv[1], "allow-key") == 0 && argc == 3)
    {
      ret = ny_revocation_update_key(argv[2], false);
    }
  else if (strcmp(argv[1], "revoke-package") == 0 && argc == 4)
    {
      ret = ny_revocation_update_package(argv[2], argv[3], true);
    }
  else if (strcmp(argv[1], "allow-package") == 0 && argc == 4)
    {
      ret = ny_revocation_update_package(argv[2], argv[3], false);
    }
  else if (strcmp(argv[1], "revocations") == 0 && argc == 2)
    {
      ret = ny_revocation_show();
    }
  else if (strcmp(argv[1], "packages") == 0 && argc == 2)
    {
      ret = ny_package_list();
    }
  else if (strcmp(argv[1], "isolation") == 0 && argc == 2)
    {
      ret = nycore_isolation();
    }
  else if (strcmp(argv[1], "list") == 0 && argc == 2)
    {
      ny_scheduler_list();
      ret = 0;
    }
  else
    {
      nycore_usage();
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "nycore: command failed: %d\n", ret);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
