/****************************************************************************
 * tools/nyabula_core/tests/mimo_test.c
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

#include "mimo_options.c"
#include "ny_mimo.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char config[2048];
static const char *host = "api.xiaomimimo.com";
static const char *thinking_level = "none";
static int scenario;
static int received;
static bool cancel;

int claw_config_get(const char *key, char *value, size_t size)
{
  const char *found = !strcmp(key, "mimo_settings")          ? config
                      : !strcmp(key, AGENT_CFG_KEY_LLM_HOST) ? host
                      : !strcmp(key, AGENT_CFG_KEY_API_KEY)  ? "fixture-key"
                      : !strcmp(key, "mimo_thinking")        ? thinking_level
                                                             : "";
  if (!found[0])
    return -1;
  snprintf(value, size, "%s", found);
  return 0;
}

int claw_config_set(const char *key, const char *value)
{
  assert(!strcmp(key, "mimo_settings"));
  assert(strlen(value) < sizeof(config));
  strcpy(config, value);
  return 0;
}

int vela_https_request_once(const char *server, const char *port,
                            const char *path, const vela_header_t *headers,
                            const char *body, char *response, size_t capacity,
                            struct vela_http_response_s *metadata,
                            vela_http_event_t event, void *context)
{
  (void)metadata;
  assert(!strcmp(server, "api.xiaomimimo.com"));
  assert(!strcmp(port, "443"));
  assert(!strcmp(path, "/v1/chat/completions"));
  assert(headers[0].value[0]);
  cJSON *request = cJSON_Parse(body);
  assert(request);
  if (!event)
    {
      if (!strcmp(ny_mimo_string(request, "model"), "mimo-v2.5-asr"))
        {
          cJSON *message =
              cJSON_GetArrayItem(cJSON_GetObjectItem(request, "messages"), 0);
          cJSON *content = cJSON_GetObjectItem(message, "content");
          assert(cJSON_GetArraySize(content) == 1);
          cJSON *part = cJSON_GetArrayItem(content, 0);
          assert(!strcmp(ny_mimo_string(part, "type"), "input_audio"));
          cJSON *audio = cJSON_GetObjectItem(part, "input_audio");
          assert(!strcmp(ny_mimo_string(audio, "format"), "wav"));
          const char *data = ny_mimo_string(audio, "data");
          unsigned char decoded[128];
          size_t size;
          assert(mbedtls_base64_decode(decoded, sizeof(decoded), &size,
                                       (const unsigned char *)data,
                                       strlen(data)) == 0);
          assert(size == 52 && !memcmp(decoded, "RIFF", 4));
          assert(decoded[24] == 0x80 && decoded[25] == 0x3e);
          assert(decoded[44] == 1 && decoded[50] == 0xff &&
                 decoded[51] == 0xff);
          assert(!cJSON_GetObjectItem(request, "thinking"));
          assert(!strcmp(
              ny_mimo_string(cJSON_GetObjectItem(request, "asr_options"),
                             "language"),
              "auto"));
          snprintf(response, capacity, "%s",
                   scenario == 6
                       ? "{\"choices\":[{\"finish_reason\":\"length\","
                         "\"message\":{\"content\":\"bad\"}}]}"
                       : "{\"choices\":[{\"finish_reason\":\"stop\","
                         "\"message\":{\"content\":\"Hello Nyabula\"}}]}");
          cJSON_Delete(request);
          return scenario == 4 ? 401 : 200;
        }
      assert(!strcmp(ny_mimo_string(request, "model"), "mimo-v2.6-flash"));
      assert(!strcmp(
          ny_mimo_string(cJSON_GetObjectItem(request, "thinking"), "type"),
          "disabled"));
      cJSON *message =
          cJSON_GetArrayItem(cJSON_GetObjectItem(request, "messages"), 0);
      assert(cJSON_GetArraySize(cJSON_GetObjectItem(message, "content")) == 2);
      snprintf(response, capacity,
               "{\"choices\":[{\"message\":{\"content\":\"A cat\"}}]}");
      cJSON_Delete(request);
      return 200;
    }
  assert(cJSON_IsTrue(cJSON_GetObjectItem(request, "stream")));
  assert(
      !strcmp(ny_mimo_string(cJSON_GetObjectItem(request, "audio"), "format"),
              "pcm16"));
  cJSON *messages = cJSON_GetObjectItem(request, "messages");
  cJSON *last = cJSON_GetArrayItem(messages, cJSON_GetArraySize(messages) - 1);
  assert(!strcmp(ny_mimo_string(last, "role"), "assistant"));
  if (strstr(ny_mimo_string(request, "model"), "voicedesign"))
    assert(
        !cJSON_GetObjectItem(cJSON_GetObjectItem(request, "audio"), "voice"));
  cJSON_Delete(request);
  if (scenario == 4)
    return 401;
  const char *audio = scenario == 1 ? "AQ==" : "AQACAAMABAA=";
  char wire[1024];
  snprintf(
      wire, sizeof(wire),
      "data: {\"choices\":[{\"delta\":{\"audio\":{\"data\":\"%s\"}}}]}\r\n\r\n"
      "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"%s\"}]}\n\n"
      "data: [DONE]\n\n",
      audio, scenario == 2 ? "length" : "stop");
  for (size_t i = 0; wire[i]; i++)
    {
      response[i] = wire[i];
      response[i + 1] = 0;
      if (wire[i] == '\n')
        {
          int ret = event(context, response, i + 1);
          if (ret)
            return ret > 0 ? 200 : -1;
        }
    }
  return 200;
}

static int pcm(void *context, const int16_t *samples, size_t count)
{
  (void)context;
  if (cancel)
    return -ECANCELED;
  for (size_t i = 0; i < count; i++)
    assert(samples[i] == (int16_t)(i + 1));
  received += (int)count;
  return 0;
}

int main(void)
{
  cJSON *result = NULL;
  const char *endpoints[] = { "api.xiaomimimo.com",
                              "token-plan-cn.xiaomimimo.com",
                              "token-plan-sgp.xiaomimimo.com",
                              "token-plan-ams.xiaomimimo.com", NULL };
  for (int i = 0; endpoints[i]; i++)
    {
      cJSON *body = cJSON_CreateObject();
      assert(llm_mimo_apply_options(body, endpoints[i]) == 0);
      assert(!strcmp(
          ny_mimo_string(cJSON_GetObjectItem(body, "thinking"), "type"),
          "disabled"));
      cJSON_Delete(body);
      thinking_level = "high";
      assert(!strcmp(llm_mimo_thinking_type(endpoints[i]), "enabled"));
      thinking_level = "none";
    }
  cJSON *other = cJSON_CreateObject();
  assert(llm_mimo_apply_options(other, "api.xiaomimimo.com.evil.test") == 0);
  assert(!cJSON_GetObjectItem(other, "thinking"));
  cJSON_Delete(other);
  assert(ny_mimo_config("agent.config.mimo.get", NULL, &result) == 0);
  assert(!cJSON_GetObjectItem(result, "key"));
  assert(!strcmp(ny_mimo_string(result, "visionModel"), "mimo-v2.6-flash"));
  assert(!strcmp(ny_mimo_string(result, "ttsModel"), "mimo-v2.5-tts"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "credentialsReady")));
  cJSON_Delete(result);
  assert(!ny_mimo_tts_enabled());
  assert(!ny_mimo_asr_enabled());
  int16_t input[] = { 1, 2, 3, -1 };
  char transcript[64];
  assert(ny_mimo_transcribe(input, 4, 16000, transcript, sizeof(transcript)) ==
         0);
  assert(!strcmp(transcript, "Hello Nyabula"));
  assert(ny_mimo_transcribe(input, 4, 16000, transcript, 3) == -EOVERFLOW);
  assert(ny_mimo_transcribe(input, 4, 24000, transcript, 64) == -EINVAL);
  assert(ny_mimo_transcribe(input, 480001, 16000, transcript, 64) == -EINVAL);
  scenario = 6;
  assert(ny_mimo_transcribe(input, 4, 16000, transcript, 64) == -EBADMSG);
  scenario = 4;
  assert(ny_mimo_transcribe(input, 4, 16000, transcript, 64) == -EACCES);
  scenario = 0;
  assert(ny_mimo_speak("Hello", pcm, NULL) == 0 && received == 4);
  scenario = 1;
  assert(ny_mimo_speak("Hello", pcm, NULL) == -EBADMSG);
  scenario = 2;
  assert(ny_mimo_speak("Hello", pcm, NULL) == -EOVERFLOW);
  scenario = 4;
  assert(ny_mimo_speak("Hello", pcm, NULL) == -EACCES);
  scenario = 0;
  cancel = true;
  assert(ny_mimo_speak("Hello", pcm, NULL) == -ECANCELED);
  cancel = false;
  char output[128];
  assert(
      ny_mimo_vision_tool(
          "{\"url\":\"https://example.com/cat.jpg\",\"prompt\":\"Describe\"}",
          output, sizeof(output)) == 0);
  assert(!strcmp(output, "A cat"));
  assert(ny_mimo_vision_tool(
             "{\"url\":\"file:///secret\",\"prompt\":\"Describe\"}", output,
             sizeof(output)) == -EINVAL);
  host = "other.example";
  assert(ny_mimo_speak("Hello", pcm, NULL) == -ENOKEY);
  cJSON *settings =
      cJSON_Parse("{\"key\":\"dedicated-fixture\",\"visionModel\":\"mimo-v2."
                  "5\",\"ttsBackend\":\"mimo\",\"ttsModel\":\"mimo-v2.5-tts-"
                  "voicedesign\",\"voice\":\"Mia\",\"style\":\"Warm\"}");
  assert(ny_mimo_config("agent.config.mimo.set", settings, &result) == 0);
  cJSON_Delete(result);
  assert(strstr(config, "mimo-v2.6-flash"));
  assert(strstr(config, "dedicated-fixture"));
  assert(ny_mimo_tts_enabled());
  assert(!ny_mimo_asr_enabled());
  cJSON_AddStringToObject(settings, "asrBackend", "mimo");
  result = NULL;
  assert(ny_mimo_config("agent.config.mimo.set", settings, &result) == 0);
  cJSON_Delete(result);
  result = NULL;
  assert(ny_mimo_asr_enabled());
  cJSON_DeleteItemFromObject(settings, "asrBackend");
  assert(ny_mimo_config("agent.config.mimo.set", settings, &result) == 0);
  assert(ny_mimo_asr_enabled());
  cJSON_Delete(result);
  result = NULL;
  assert(ny_mimo_speak("Hello", pcm, NULL) == 0);
  cJSON_ReplaceItemInObject(settings, "key", cJSON_CreateString(""));
  assert(ny_mimo_config("agent.config.mimo.set", settings, &result) == 0);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result, "keySet")));
  cJSON_Delete(result);
  cJSON_ReplaceItemInObject(settings, "style", cJSON_CreateString(""));
  assert(ny_mimo_config("agent.config.mimo.set", settings, &result) ==
         -EINVAL);
  cJSON_Delete(settings);
  assert(ny_mimo_host_supported("token-plan-cn.xiaomimimo.com"));
  assert(ny_mimo_host_supported("token-plan-sgp.xiaomimimo.com"));
  assert(!ny_mimo_host_supported("api.xiaomimimo.com.evil.test"));
  assert(!ny_mimo_host_supported(NULL));
  strcpy(config, "{\"visionModel\":\"mimo-v2.5\",\"key\":\"legacy-fixture\","
                 "\"ttsBackend\":\"local\",\"ttsModel\":\"mimo-v2.5-tts\","
                 "\"voice\":\"mimo_default\"}");
  char original[2048];
  strcpy(original, config);
  assert(ny_mimo_config("agent.config.mimo.get", NULL, &result) == 0);
  assert(!strcmp(ny_mimo_string(result, "visionModel"), "mimo-v2.6-flash"));
  assert(!strcmp(ny_mimo_string(result, "ttsModel"), "mimo-v2.5-tts"));
  assert(!strcmp(config, original));
  assert(!cJSON_GetObjectItem(result, "key"));
  cJSON_Delete(result);
  strcpy(config, "{");
  assert(ny_mimo_config("agent.config.mimo.get", NULL, &result) == -EBADMSG);
  config[0] = 0;
  puts("MIMO_PROTOCOL_CONFIG_AUDIO_PASS");
  return 0;
}
