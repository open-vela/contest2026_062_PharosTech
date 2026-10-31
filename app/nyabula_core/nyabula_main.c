/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/nyabula_main.c
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

#include "ny_product.h"
#include <errno.h>
#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#ifdef CONFIG_NYABULA_CORE_WEB
#include "ny_web.h"
#include "ny_web_auth.h"
#endif

struct ny_cli_input_s
{
  bool skip_lf;
};

struct ny_cli_route_s
{
  const char *group;
  const char *action;
  const char *topic;
  const char *field;
  const char *value;
};

/* NULL field means no arguments; NULL value means one string argument. */

static const struct ny_cli_route_s g_routes[] = {
  { "status", "", "sys.info", NULL, NULL },
  { "device", "status", "device.status", NULL, NULL },
  { "network", "status", "network.status", NULL, NULL },
  { "network", "scan", "network.wifi.scan", NULL, NULL },
  { "network", "results", "network.wifi.scan.result", NULL, NULL },
  { "network", "name", "network.name.set", "name", NULL },
  { "network", "provision", "network.provision.start", NULL, NULL },
  { "network", "stop-provision", "network.provision.stop", NULL, NULL },
  { "model", "status", "agent.config.get", NULL, NULL },
  { "mimo", "status", "agent.config.mimo.get", NULL, NULL },
  { "thinking", "status", "agent.config.thinking.get", NULL, NULL },
  { "thinking", "set", "agent.config.thinking.set", "level", NULL },
  { "voice", "status", "voice.status", NULL, NULL },
  { "voice", "on", "voice.enable", "enabled", "true" },
  { "voice", "off", "voice.enable", "enabled", "false" },
  { "voice", "listen", "voice.listen", NULL, NULL },
  { "voice", "cancel", "voice.cancel", NULL, NULL },
  { "voice", "say", "voice.say", "text", NULL },
  { "eyes", "status", "eyes.status", NULL, NULL },
  { "service", "status", "product.status", NULL, NULL },
  { "service", "start", "product.start", NULL, NULL },
  { "service", "stop", "product.stop", NULL, NULL },
  { "storage", "status", "storage.status", NULL, NULL },
  { "update", "status", "update.status", NULL, NULL },
  { "time", "status", "system.time.get", NULL, NULL },
  { "time", "sync", "system.time.sync", NULL, NULL },
  { "logs", "", "logs.tail", NULL, NULL }
};

static void ny_cli_usage(void);
static int ny_cli_request(const char *topic, cJSON *data);
static int ny_cli_line(struct ny_cli_input_s *input, const char *prompt,
                       char *out, size_t size, bool secret);
static int ny_cli_dispatch(struct ny_cli_input_s *input, int argc,
                           char **argv);
static int ny_cli_menu(struct ny_cli_input_s *input);
#ifdef CONFIG_NYABULA_CORE_WEB
static int ny_cli_password(struct ny_cli_input_s *input);
#endif

/****************************************************************************
 * Name: ny_cli_usage
 ****************************************************************************/

static void ny_cli_usage(void)
{
  puts("Nyabula device manager\n"
       "  nyabula                         interactive menu\n"
       "  nyabula status | logs\n"
       "  nyabula panel status | reset-password | qr | hide\n"
       "  nyabula network status | scan | results | provision | "
       "stop-provision\n"
       "  nyabula network name <name>\n"
       "  nyabula network connect <ssid>  prompt for Wi-Fi password\n"
       "  nyabula model status | configure <request.json>\n"
       "  nyabula mimo status | configure <request.json>\n"
       "  nyabula thinking status | set "
       "<none|enabled|minimal|low|medium|high|xhigh|max|ultra>\n"
       "  nyabula voice status | on | off | listen | cancel\n"
       "  nyabula voice say <text>\n"
       "  nyabula voice configure <request.json>\n"
       "  nyabula service status | start | stop\n"
       "  nyabula device|eyes|storage|update status\n"
       "  nyabula time status | sync\n"
       "  nyabula request <topic> [request.json]\n"
       "  nyabula help\n"
       "Configuration uses the same request fields as the panel.\n"
       "Password reset needs no old password; enter the new one twice.");
}

/****************************************************************************
 * Name: ny_cli_request
 ****************************************************************************/

static int ny_cli_request(const char *topic, cJSON *data)
{
  const struct ny_product_caller_s caller = { "nyabula-cli", NY_PRODUCT_OWNER,
                                              true };
  cJSON *result = NULL;
  char *text;
  int ret;

  if (data == NULL)
    return -ENOMEM;
  ret = ny_product_request(&caller, topic, data, &result);
  cJSON_Delete(data);
  if (result != NULL)
    {
      text = cJSON_Print(result);
      if (text == NULL && ret == 0)
        ret = -ENOMEM;
      else if (text != NULL)
        {
          puts(text);
          cJSON_free(text);
        }
    }
  cJSON_Delete(result);
  return ret;
}

/****************************************************************************
 * Name: ny_cli_line
 ****************************************************************************/

static int ny_cli_line(struct ny_cli_input_s *input, const char *prompt,
                       char *out, size_t size, bool secret)
{
  struct termios saved;
  struct termios quiet;
  bool changed = false;
  bool overflow = false;
  size_t length = 0;
  int ret = 0;
  int ch;

  if (isatty(STDIN_FILENO))
    {
      if (tcgetattr(STDIN_FILENO, &saved) < 0)
        return -errno;
      quiet = saved;
      quiet.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG);
      quiet.c_iflag &= ~(ICRNL | INLCR | IGNCR);
      quiet.c_cc[VMIN] = 1;
      quiet.c_cc[VTIME] = 0;
      if (tcsetattr(STDIN_FILENO, TCSANOW, &quiet) < 0)
        return -errno;
      changed = true;
    }
  fputs(prompt, stdout);
  fflush(stdout);
  for (;;)
    {
      ch = getchar();
      if (input->skip_lf)
        {
          input->skip_lf = false;
          if (ch == '\n')
            continue;
        }
      if (ch == EOF || ch == 3 || ch == 4)
        {
          ret = ch == EOF && length > 0 ? 0 : -ECANCELED;
          break;
        }
      if (ch == '\r' || ch == '\n')
        {
          input->skip_lf = ch == '\r';
          break;
        }
      if (ch == '\b' || ch == 127)
        {
          if (length > 0 && !overflow)
            {
              length--;
              if (!secret)
                fputs("\b \b", stdout);
            }
        }
      else if (length + 1 < size && !overflow)
        {
          out[length++] = ch;
          if (!secret)
            putchar(ch);
        }
      else
        overflow = true;
      fflush(stdout);
    }
  out[length] = 0;
  if (overflow)
    ret = -E2BIG;
  if (changed && tcsetattr(STDIN_FILENO, TCSANOW, &saved) < 0 && ret == 0)
    ret = -errno;
  putchar('\n');
  return ret;
}

#ifdef CONFIG_NYABULA_CORE_WEB
/****************************************************************************
 * Name: ny_cli_password
 ****************************************************************************/

static int ny_cli_password(struct ny_cli_input_s *input)
{
  char first[NY_WEB_AUTH_PASSWORD_MAX + 3] = { 0 };
  char second[sizeof(first)] = { 0 };
  int ret = ny_cli_line(input, "New panel password (8-64 bytes): ", first,
                        sizeof(first), true);
  if (ret == 0)
    ret =
        ny_cli_line(input, "Repeat password: ", second, sizeof(second), true);
  if (ret == 0 && strcmp(first, second))
    {
      fputs("Passwords do not match. Nothing changed.\n", stderr);
      ret = -EINVAL;
    }
  if (ret == 0)
    {
      ret = ny_web_auth_set(first);
      if (ret == -EAGAIN)
        {
          ret = ny_web_auth_load(CONFIG_NYABULA_CORE_WEB_AUTH_PATH);
          if (ret == 0)
            ret = ny_web_auth_set(first);
        }
    }
  memset(first, 0, sizeof(first));
  memset(second, 0, sizeof(second));
  if (ret == 0)
    puts("Panel password saved. Use the new password to sign in.");
  return ret;
}
#endif

/****************************************************************************
 * Name: ny_cli_dispatch
 ****************************************************************************/

static int ny_cli_dispatch(struct ny_cli_input_s *input, int argc, char **argv)
{
  cJSON *data;
  int ret;
  if (argc == 2 && !strcmp(argv[1], "help"))
    {
      ny_cli_usage();
      return 0;
    }
#ifdef CONFIG_NYABULA_CORE_WEB
  if (argc == 3 && !strcmp(argv[1], "panel"))
    {
      if (!strcmp(argv[2], "reset-password"))
        return ny_cli_password(input);
      if (!strcmp(argv[2], "status"))
        {
          uint32_t retry;
          bool locked = ny_web_auth_locked(&retry);
          printf("password_set=%s locked=%s retry_ms=%lu panels=%d\n",
                 ny_web_auth_has_password() ? "yes" : "no",
                 locked ? "yes" : "no", (unsigned long)retry,
                 ny_web_panel_count());
          return 0;
        }
      if (!strcmp(argv[2], "hide"))
        return ny_cli_request("panel.pair.hide", cJSON_CreateObject());
      if (!strcmp(argv[2], "qr"))
        return ny_cli_request("panel.pair.show", cJSON_CreateObject());
    }
#endif
  if (!strcmp(argv[1], "request") && (argc == 3 || argc == 4))
    return argc == 4 ? ny_product_cli(argv[2], argv[3])
                     : ny_cli_request(argv[2], cJSON_CreateObject());
  if (argc == 4 && !strcmp(argv[2], "configure"))
    {
      const char *topic = !strcmp(argv[1], "model")   ? "agent.config.set"
                          : !strcmp(argv[1], "mimo")  ? "agent.config.mimo.set"
                          : !strcmp(argv[1], "voice") ? "voice.config"
                                                      : NULL;
      if (topic != NULL)
        return ny_product_cli(topic, argv[3]);
    }
  if (argc == 4 && !strcmp(argv[1], "network") && !strcmp(argv[2], "connect"))
    {
      char password[66] = { 0 };
      ret = ny_cli_line(input,
                        "Wi-Fi password (empty for open network): ", password,
                        sizeof(password), true);
      data = cJSON_CreateObject();
      if (ret == 0 &&
          (!data || !cJSON_AddStringToObject(data, "ssid", argv[3]) ||
           !cJSON_AddStringToObject(data, "psk", password)))
        ret = -ENOMEM;
      memset(password, 0, sizeof(password));
      if (ret < 0)
        {
          cJSON_Delete(data);
          return ret;
        }
      return ny_cli_request("network.wifi.set", data);
    }
  for (size_t i = 0; i < sizeof(g_routes) / sizeof(g_routes[0]); i++)
    {
      const struct ny_cli_route_s *route = &g_routes[i];
      int base = route->action[0] ? 3 : 2;
      int expected = base + (route->field != NULL && route->value == NULL);
      if (argc != expected || strcmp(argv[1], route->group) ||
          (base == 3 && strcmp(argv[2], route->action)))
        continue;
      data = cJSON_CreateObject();
      if (data == NULL)
        return -ENOMEM;
      if (route->field != NULL &&
          (route->value != NULL
               ? cJSON_AddBoolToObject(data, route->field,
                                       !strcmp(route->value, "true"))
               : cJSON_AddStringToObject(data, route->field, argv[base])) ==
              NULL)
        {
          cJSON_Delete(data);
          return -ENOMEM;
        }
      return ny_cli_request(route->topic, data);
    }
  ny_cli_usage();
  return -EINVAL;
}

/****************************************************************************
 * Name: ny_cli_menu
 ****************************************************************************/

static int ny_cli_menu(struct ny_cli_input_s *input)
{
  static const char *const groups[] = { "status",  "panel", "panel", "network",
                                        "model",   "mimo",  "voice", "service",
                                        "storage", "logs",  "help" };
  static const char *const actions[] = {
    "",       "reset-password", "qr",     "status", "status", "status",
    "status", "status",         "status", "",       ""
  };
  char choice_text[16];
  char *end;
  long choice;
  int ret;
  for (;;)
    {
      puts("\nNyabula\n1 Status\n2 Reset panel password\n3 Show login QR\n"
           "4 Network\n5 Model\n6 MiMo\n7 Voice\n8 Services\n"
           "9 Storage\n10 Logs\n11 All commands\n0 Exit");
      ret = ny_cli_line(input, "Select: ", choice_text, sizeof(choice_text),
                        false);
      if (ret < 0)
        return ret;
      choice = strtol(choice_text, &end, 10);
      if (end == choice_text || *end || choice < 0 || choice > 11)
        continue;
      if (choice == 0)
        return 0;
      char *args[] = { "nyabula", (char *)groups[choice - 1],
                       (char *)actions[choice - 1] };
      ret = ny_cli_dispatch(input, actions[choice - 1][0] ? 3 : 2, args);
      if (ret < 0)
        fprintf(stderr, "nyabula: command failed: %d\n", ret);
    }
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, char **argv)
{
  struct ny_cli_input_s input = { 0 };
  int ret =
      argc == 1 ? ny_cli_menu(&input) : ny_cli_dispatch(&input, argc, argv);
  if (ret < 0)
    fprintf(stderr, "nyabula: command failed: %d\n", ret);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
