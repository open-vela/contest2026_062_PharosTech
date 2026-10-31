/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_mimo.c
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

#include "ny_mimo.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "mbedtls/base64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define NY_MIMO_HOST         "api.xiaomimimo.com"
#define NY_MIMO_PATH         "/v1/chat/completions"
#define NY_MIMO_RESPONSE_MAX (4U * 1024U * 1024U)
#define NY_MIMO_PCM_BLOCK    512U

struct ny_mimo_settings_s
{
  char key[128];
  char host[64];
  char vision[64];
  char backend[16];
  char asr_backend[16];
  char model[64];
  char voice[32];
  char style[1024];
};

struct ny_mimo_stream_s
{
  ny_mimo_pcm_t callback;
  void *context;
  size_t consumed;
  bool finished;
  bool audio;
  int error;
};

static const char *ny_mimo_string(const cJSON *data, const char *key);
static int ny_mimo_load(struct ny_mimo_settings_s *settings);
static int ny_mimo_credentials(struct ny_mimo_settings_s *settings);
static bool ny_mimo_one_of(const char *value, const char *const *choices);
static int ny_mimo_stream(void *context, const char *body, size_t length);
static int ny_mimo_pcm(struct ny_mimo_stream_s *stream, const char *encoded);
static int ny_mimo_request(struct ny_mimo_settings_s *settings, cJSON *body,
                           char *response, size_t capacity,
                           vela_http_event_t event, void *context);
static void ny_mimo_le32(unsigned char *output, uint32_t value);

/****************************************************************************
 * Name: ny_mimo_host_supported
 ****************************************************************************/

bool ny_mimo_host_supported(const char *host)
{
  return host && (!strcmp(host, "api.xiaomimimo.com") ||
                  !strcmp(host, "token-plan-cn.xiaomimimo.com") ||
                  !strcmp(host, "token-plan-sgp.xiaomimimo.com") ||
                  !strcmp(host, "token-plan-ams.xiaomimimo.com"));
}

/****************************************************************************
 * Name: ny_mimo_string
 ****************************************************************************/

static const char *ny_mimo_string(const cJSON *data, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

/****************************************************************************
 * Name: ny_mimo_one_of
 ****************************************************************************/

static bool ny_mimo_one_of(const char *value, const char *const *choices)
{
  for (; *choices; choices++)
    if (!strcmp(value, *choices))
      return true;
  return false;
}

/****************************************************************************
 * Name: ny_mimo_load
 ****************************************************************************/

static int ny_mimo_load(struct ny_mimo_settings_s *settings)
{
  memset(settings, 0, sizeof(*settings));
  strcpy(settings->host, NY_MIMO_HOST);
  strcpy(settings->vision, "mimo-v2.6-flash");
  strcpy(settings->backend, "local");
  strcpy(settings->asr_backend, "local");
  strcpy(settings->model, "mimo-v2.5-tts");
  strcpy(settings->voice, "mimo_default");
  char encoded[2048] = "";
  if (claw_config_get("mimo_settings", encoded, sizeof(encoded)) != 0)
    return 0;
  cJSON *root = cJSON_Parse(encoded);
  if (!cJSON_IsObject(root))
    {
      cJSON_Delete(root);
      return -EBADMSG;
    }
#define NY_MIMO_LOAD(field, name)                          \
  snprintf(settings->field, sizeof(settings->field), "%s", \
           ny_mimo_string(root, name))
  NY_MIMO_LOAD(key, "key");
  if (ny_mimo_string(root, "host")[0])
    {
      NY_MIMO_LOAD(host, "host");
    }
  NY_MIMO_LOAD(vision, "visionModel");
  /* Retired vision settings remain readable without rewriting credentials. */

  if (!strcmp(settings->vision, "mimo-v2.5"))
    strcpy(settings->vision, "mimo-v2.6-flash");
  NY_MIMO_LOAD(backend, "ttsBackend");
  if (ny_mimo_string(root, "asrBackend")[0])
    {
      NY_MIMO_LOAD(asr_backend, "asrBackend");
    }
  NY_MIMO_LOAD(model, "ttsModel");
  NY_MIMO_LOAD(voice, "voice");
  NY_MIMO_LOAD(style, "style");
#undef NY_MIMO_LOAD
  cJSON_Delete(root);
  memset(encoded, 0, sizeof(encoded));
  return 0;
}

/****************************************************************************
 * Name: ny_mimo_credentials
 ****************************************************************************/

static int ny_mimo_credentials(struct ny_mimo_settings_s *settings)
{
  if (!ny_mimo_host_supported(settings->host))
    return -EINVAL;
  if (settings->key[0])
    return 0;
  char host[128] = "";
  claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host));
  if (strcmp(host, settings->host) || !ny_mimo_host_supported(host))
    return -ENOKEY;
  return claw_config_get(AGENT_CFG_KEY_API_KEY, settings->key,
                         sizeof(settings->key)) == 0 &&
                 settings->key[0]
             ? 0
             : -ENOKEY;
}

/****************************************************************************
 * Name: ny_mimo_config
 ****************************************************************************/

int ny_mimo_config(const char *topic, const cJSON *data, cJSON **result)
{
  static const char *const models[] = { "mimo-v2.5-tts",
                                        "mimo-v2.5-tts-voicedesign", NULL };
  static const char *const voices[] = {
    "mimo_default", "冰糖",  "茉莉", "苏打", "白桦",
    "Mia",          "Chloe", "Milo", "Dean", NULL
  };
  static const char *const vision[] = { "mimo-v2.5", "mimo-v2.6-flash",
                                        "mimo-v2.6-pro",
                                        "mimo-v2.6-pro-ultraspeed", NULL };
  struct ny_mimo_settings_s settings;
  int ret = ny_mimo_load(&settings);
  if (ret < 0)
    return ret;
  if (!strcmp(topic, "agent.config.mimo.set"))
    {
      const char *backend = ny_mimo_string(data, "ttsBackend");
      const cJSON *asr = cJSON_GetObjectItemCaseSensitive(data, "asrBackend");
      const char *model = ny_mimo_string(data, "ttsModel");
      const char *voice = ny_mimo_string(data, "voice");
      const char *style = ny_mimo_string(data, "style");
      const char *key = ny_mimo_string(data, "key");
      const char *host = ny_mimo_string(data, "host");
      if ((host[0] && !ny_mimo_host_supported(host)) ||
          (asr &&
           (!cJSON_IsString(asr) || (strcmp(asr->valuestring, "local") &&
                                     strcmp(asr->valuestring, "mimo")))) ||
          (strcmp(backend, "local") && strcmp(backend, "mimo")) ||
          !ny_mimo_one_of(model, models) || !ny_mimo_one_of(voice, voices) ||
          !ny_mimo_one_of(ny_mimo_string(data, "visionModel"), vision) ||
          strlen(style) >= sizeof(settings.style) ||
          strlen(key) >= sizeof(settings.key) ||
          (!strcmp(model, "mimo-v2.5-tts-voicedesign") && !style[0]))
        return -EINVAL;
      for (const char *p = key; *p; p++)
        if ((unsigned char)*p < 33 || (unsigned char)*p > 126)
          return -EINVAL;
      /* An endpoint change must not carry a dedicated credential with it. */

      if (host[0] && strcmp(host, settings.host) && !key[0] &&
          settings.key[0] &&
          !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "clearKey")))
        return -EINVAL;
      cJSON *saved = cJSON_Duplicate(data, true);
      if (saved && !asr &&
          !cJSON_AddStringToObject(saved, "asrBackend", settings.asr_backend))
        {
          cJSON_Delete(saved);
          return -ENOMEM;
        }
      if (!saved)
        return -ENOMEM;
      if (!strcmp(ny_mimo_string(saved, "visionModel"), "mimo-v2.5"))
        {
          cJSON *model_name = cJSON_CreateString("mimo-v2.6-flash");
          if (!model_name || !cJSON_ReplaceItemInObjectCaseSensitive(
                                 saved, "visionModel", model_name))
            {
              cJSON_Delete(model_name);
              cJSON_Delete(saved);
              return -ENOMEM;
            }
        }
      cJSON_DeleteItemFromObjectCaseSensitive(saved, "key");
      bool clear =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "clearKey"));
      if (!cJSON_AddStringToObject(saved, "key",
                                   clear    ? ""
                                   : key[0] ? key
                                            : settings.key))
        ret = -ENOMEM;
      char *encoded = ret == 0 ? cJSON_PrintUnformatted(saved) : NULL;
      cJSON_Delete(saved);
      if (!encoded)
        return -ENOMEM;
      ret = strlen(encoded) >= 2048                          ? -E2BIG
            : claw_config_set("mimo_settings", encoded) == 0 ? 0
                                                             : -EIO;
      memset(encoded, 0, strlen(encoded));
      free(encoded);
      if (ret < 0)
        return ret;
      ret = ny_mimo_load(&settings);
    }
  else if (strcmp(topic, "agent.config.mimo.get"))
    return -ENOSYS;
  cJSON *root = cJSON_CreateObject();
  bool valid =
      ret == 0 && root &&
      cJSON_AddStringToObject(root, "host", settings.host) &&
      cJSON_AddStringToObject(root, "visionModel", settings.vision) &&
      cJSON_AddStringToObject(root, "ttsBackend", settings.backend) &&
      cJSON_AddStringToObject(root, "asrBackend", settings.asr_backend) &&
      cJSON_AddStringToObject(root, "asrModel", "mimo-v2.5-asr") &&
      cJSON_AddStringToObject(root, "ttsModel", settings.model) &&
      cJSON_AddStringToObject(root, "voice", settings.voice) &&
      cJSON_AddStringToObject(root, "style", settings.style) &&
      cJSON_AddBoolToObject(root, "keySet", settings.key[0] != 0) &&
      cJSON_AddBoolToObject(root, "credentialsReady",
                            ny_mimo_credentials(&settings) == 0);
  memset(&settings, 0, sizeof(settings));
  if (!valid)
    {
      cJSON_Delete(root);
      return ret < 0 ? ret : -ENOMEM;
    }
  *result = root;
  return 0;
}

/****************************************************************************
 * Name: ny_mimo_tts_enabled
 ****************************************************************************/

bool ny_mimo_tts_enabled(void)
{
  struct ny_mimo_settings_s settings;
  bool enabled =
      ny_mimo_load(&settings) == 0 && !strcmp(settings.backend, "mimo");
  memset(&settings, 0, sizeof(settings));
  return enabled;
}

/****************************************************************************
 * Name: ny_mimo_asr_enabled
 ****************************************************************************/

bool ny_mimo_asr_enabled(void)
{
  struct ny_mimo_settings_s settings;
  bool enabled =
      ny_mimo_load(&settings) == 0 && !strcmp(settings.asr_backend, "mimo");
  memset(&settings, 0, sizeof(settings));
  return enabled;
}

/****************************************************************************
 * Name: ny_mimo_le32
 ****************************************************************************/

static void ny_mimo_le32(unsigned char *output, uint32_t value)
{
  for (unsigned int i = 0; i < 4; i++)
    output[i] = (unsigned char)(value >> (i * 8));
}

/****************************************************************************
 * Name: ny_mimo_transcribe
 * Description: Submit one bounded mono PCM capture as an in-memory WAV.
 ****************************************************************************/

int ny_mimo_transcribe(const int16_t *pcm, size_t samples, unsigned int rate,
                       char *text, size_t capacity)
{
  if (!pcm || !samples || rate != 16000 || samples > rate * 30U || !text ||
      capacity < 2)
    return -EINVAL;
  text[0] = '\0';
  struct ny_mimo_settings_s settings;
  int ret = ny_mimo_load(&settings);
  if (ret < 0)
    return ret;
  size_t wav_size = 44 + samples * 2;
  size_t encoded_size = 4 * ((wav_size + 2) / 3) + 1;
  unsigned char *wav = calloc(1, wav_size);
  char *encoded = malloc(encoded_size);
  char *response = calloc(1, 32768);
  cJSON *body = cJSON_CreateObject();
  cJSON *messages = cJSON_AddArrayToObject(body, "messages");
  cJSON *message = cJSON_CreateObject();
  if (!wav || !encoded || !response || !body || !messages || !message)
    {
      ret = -ENOMEM;
      cJSON_Delete(message);
      goto out;
    }
  if (!cJSON_AddItemToArray(messages, message))
    {
      cJSON_Delete(message);
      ret = -ENOMEM;
      goto out;
    }
  memcpy(wav, "RIFF", 4);
  ny_mimo_le32(wav + 4, (uint32_t)wav_size - 8);
  memcpy(wav + 8, "WAVEfmt ", 8);
  ny_mimo_le32(wav + 16, 16);
  wav[20] = 1;
  wav[22] = 1;
  ny_mimo_le32(wav + 24, rate);
  ny_mimo_le32(wav + 28, rate * 2);
  wav[32] = 2;
  wav[34] = 16;
  memcpy(wav + 36, "data", 4);
  ny_mimo_le32(wav + 40, (uint32_t)samples * 2);
  for (size_t i = 0; i < samples; i++)
    {
      wav[44 + i * 2] = (unsigned char)pcm[i];
      wav[45 + i * 2] = (unsigned char)((uint16_t)pcm[i] >> 8);
    }
  size_t written;
  if (mbedtls_base64_encode((unsigned char *)encoded, encoded_size, &written,
                            wav, wav_size) != 0)
    {
      ret = -EIO;
      goto out;
    }
  encoded[written] = '\0';
  cJSON *content = cJSON_AddArrayToObject(message, "content");
  cJSON *part = cJSON_CreateObject();
  if (!content || !part || !cJSON_AddItemToArray(content, part))
    {
      cJSON_Delete(part);
      ret = -ENOMEM;
      goto out;
    }
  cJSON *audio = cJSON_AddObjectToObject(part, "input_audio");
  cJSON *options = cJSON_AddObjectToObject(body, "asr_options");
  if (!audio || !options ||
      !cJSON_AddStringToObject(body, "model", "mimo-v2.5-asr") ||
      !cJSON_AddBoolToObject(body, "stream", false) ||
      !cJSON_AddStringToObject(message, "role", "user") ||
      !cJSON_AddStringToObject(part, "type", "input_audio") ||
      !cJSON_AddStringToObject(audio, "format", "wav") ||
      !cJSON_AddStringToObject(audio, "data", encoded) ||
      !cJSON_AddStringToObject(options, "language", "auto"))
    {
      ret = -ENOMEM;
      goto out;
    }
  ret = ny_mimo_request(&settings, body, response, 32768, NULL, NULL);
  if (ret == 0)
    {
      cJSON *root = cJSON_Parse(response);
      cJSON *choice = cJSON_GetArrayItem(
          cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
      cJSON *answer = cJSON_GetObjectItemCaseSensitive(
          cJSON_GetObjectItemCaseSensitive(choice, "message"), "content");
      if (!cJSON_IsString(answer) ||
          strcmp(ny_mimo_string(choice, "finish_reason"), "stop"))
        ret = -EBADMSG;
      else if (strlen(answer->valuestring) >= capacity)
        ret = -EOVERFLOW;
      else
        strcpy(text, answer->valuestring);
      cJSON_Delete(root);
    }
out:
  memset(&settings, 0, sizeof(settings));
  free(wav);
  free(encoded);
  free(response);
  cJSON_Delete(body);
  return ret;
}

/****************************************************************************
 * Name: ny_mimo_request
 ****************************************************************************/

static int ny_mimo_request(struct ny_mimo_settings_s *settings, cJSON *body,
                           char *response, size_t capacity,
                           vela_http_event_t event, void *context)
{
  int ret = ny_mimo_credentials(settings);
  if (ret < 0)
    return ret;
  char *encoded = cJSON_PrintUnformatted(body);
  if (!encoded)
    return -ENOMEM;
  vela_header_t headers[] = { { "api-key", settings->key },
                              { "Content-Type", "application/json" },
                              { NULL, NULL } };
  struct vela_http_response_s metadata;
  ret = vela_https_request_once(settings->host, "443", NY_MIMO_PATH, headers,
                                encoded, response, capacity, &metadata, event,
                                context);
  free(encoded);
  if (ret != 200)
    syslog(LOG_WARNING, "nymimo: request status=%d\n", ret);
  return ret == 200   ? 0
         : ret < 0    ? -EIO
         : ret == 401 ? -EACCES
         : ret == 429 ? -EAGAIN
                      : -EPROTO;
}

/****************************************************************************
 * Name: ny_mimo_pcm
 ****************************************************************************/

static int ny_mimo_pcm(struct ny_mimo_stream_s *stream, const char *encoded)
{
  size_t size = strlen(encoded);
  size_t decoded = 0;
  if (size == 0)
    return 0;
  if (size > NY_MIMO_RESPONSE_MAX || size % 4)
    return -EBADMSG;
  unsigned char *pcm = malloc(size / 4 * 3 + 1);
  if (!pcm)
    return -ENOMEM;
  int ret = mbedtls_base64_decode(pcm, size / 4 * 3 + 1, &decoded,
                                  (const unsigned char *)encoded, size);
  if (ret == 0 && decoded % 2)
    ret = -EBADMSG;
  for (size_t offset = 0; ret == 0 && offset < decoded;)
    {
      int16_t samples[NY_MIMO_PCM_BLOCK];
      size_t count = (decoded - offset) / 2;
      if (count > NY_MIMO_PCM_BLOCK)
        count = NY_MIMO_PCM_BLOCK;
      for (size_t i = 0; i < count; i++)
        samples[i] = (int16_t)((uint16_t)pcm[offset + i * 2] |
                               (uint16_t)pcm[offset + i * 2 + 1] << 8);
      ret = stream->callback(stream->context, samples, count);
      offset += count * 2;
    }
  free(pcm);
  if (ret == 0 && decoded)
    stream->audio = true;
  return ret;
}

/****************************************************************************
 * Name: ny_mimo_stream
 ****************************************************************************/

static int ny_mimo_stream(void *context, const char *body, size_t length)
{
  struct ny_mimo_stream_s *stream = context;
  const char *line = body + stream->consumed;
  size_t count = length - stream->consumed;
  stream->consumed = length;
  if (stream->callback(stream->context, NULL, 0) < 0)
    return stream->error = -ECANCELED;
  if (count < 6 || strncmp(line, "data:", 5))
    return 0;
  line += 5;
  while (*line == ' ')
    line++;
  if (!strncmp(line, "[DONE]", 6))
    return stream->finished && stream->audio ? 1 : (stream->error = -EBADMSG);
  cJSON *root = cJSON_Parse(line);
  if (!root || cJSON_GetObjectItemCaseSensitive(root, "error"))
    {
      cJSON_Delete(root);
      return stream->error = -EBADMSG;
    }
  cJSON *choice =
      cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
  cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
  const char *audio =
      ny_mimo_string(cJSON_GetObjectItemCaseSensitive(delta, "audio"), "data");
  int ret = ny_mimo_pcm(stream, audio);
  const char *reason = ny_mimo_string(choice, "finish_reason");
  if (reason[0])
    {
      stream->finished = !strcmp(reason, "stop");
      if (!stream->finished)
        ret = -EOVERFLOW;
    }
  cJSON_Delete(root);
  if (ret < 0)
    stream->error = ret;
  return ret;
}

/****************************************************************************
 * Name: ny_mimo_speak
 ****************************************************************************/

int ny_mimo_speak(const char *text, ny_mimo_pcm_t callback, void *context)
{
  if (!text || !text[0] || strlen(text) > 1024 || !callback)
    return -EINVAL;
  struct ny_mimo_settings_s settings;
  int ret = ny_mimo_load(&settings);
  if (ret < 0)
    return ret;
  cJSON *body = cJSON_CreateObject();
  cJSON *messages = cJSON_AddArrayToObject(body, "messages");
  cJSON *audio = cJSON_AddObjectToObject(body, "audio");
  bool valid = body && messages && audio &&
               cJSON_AddStringToObject(body, "model", settings.model) &&
               cJSON_AddBoolToObject(body, "stream", true) &&
               cJSON_AddStringToObject(audio, "format", "pcm16");
  if (!strcmp(settings.model, "mimo-v2.5-tts"))
    valid &= cJSON_AddStringToObject(audio, "voice", settings.voice) != NULL;
  if (settings.style[0])
    {
      cJSON *style = cJSON_CreateObject();
      valid &= style && cJSON_AddStringToObject(style, "role", "user") &&
               cJSON_AddStringToObject(style, "content", settings.style);
      if (!cJSON_AddItemToArray(messages, style))
        {
          cJSON_Delete(style);
          valid = false;
        }
    }
  cJSON *message = cJSON_CreateObject();
  valid &= message && cJSON_AddStringToObject(message, "role", "assistant") &&
           cJSON_AddStringToObject(message, "content", text);
  if (!cJSON_AddItemToArray(messages, message))
    {
      cJSON_Delete(message);
      valid = false;
    }
  char *response = valid ? malloc(NY_MIMO_RESPONSE_MAX) : NULL;
  struct ny_mimo_stream_s stream = { .callback = callback,
                                     .context = context };
  ret = !response
            ? -ENOMEM
            : ny_mimo_request(&settings, body, response, NY_MIMO_RESPONSE_MAX,
                              ny_mimo_stream, &stream);
  if (stream.error)
    ret = stream.error;
  else if (ret == 0 && (!stream.finished || !stream.audio))
    ret = -EBADMSG;
  free(response);
  cJSON_Delete(body);
  memset(&settings, 0, sizeof(settings));
  return ret;
}

/****************************************************************************
 * Name: ny_mimo_vision_tool
 ****************************************************************************/

int ny_mimo_vision_tool(const char *input, char *output, size_t capacity)
{
  cJSON *args = cJSON_Parse(input);
  const char *url = ny_mimo_string(args, "url");
  const char *prompt = ny_mimo_string(args, "prompt");
  if (strncmp(url, "https://", 8) || strlen(url) > 1536 || !prompt[0] ||
      strlen(prompt) > 1024)
    {
      cJSON_Delete(args);
      snprintf(output, capacity,
               "Provide a public HTTPS image URL and prompt.");
      return -EINVAL;
    }
  struct ny_mimo_settings_s settings;
  int ret = ny_mimo_load(&settings);
  cJSON *body = cJSON_CreateObject();
  cJSON *messages = cJSON_AddArrayToObject(body, "messages");
  cJSON *message = cJSON_CreateObject();
  cJSON *content = cJSON_AddArrayToObject(message, "content");
  cJSON *text = cJSON_CreateObject();
  cJSON *image = cJSON_CreateObject();
  cJSON *image_url = cJSON_AddObjectToObject(image, "image_url");
  char level[16] = "enabled";
  claw_config_get("mimo_thinking", level, sizeof(level));
  cJSON *thinking = cJSON_AddObjectToObject(body, "thinking");
  bool valid =
      ret == 0 && body && messages && message && content && text && image &&
      image_url && thinking &&
      cJSON_AddStringToObject(body, "model", settings.vision) &&
      cJSON_AddNumberToObject(body, "max_completion_tokens", 8192) &&
      cJSON_AddStringToObject(
          thinking, "type", !strcmp(level, "none") ? "disabled" : "enabled") &&
      cJSON_AddStringToObject(message, "role", "user") &&
      cJSON_AddStringToObject(text, "type", "text") &&
      cJSON_AddStringToObject(text, "text", prompt) &&
      cJSON_AddStringToObject(image, "type", "image_url") &&
      cJSON_AddStringToObject(image_url, "url", url);
  if (!cJSON_AddItemToArray(content, text))
    {
      cJSON_Delete(text);
      valid = false;
    }
  if (!cJSON_AddItemToArray(content, image))
    {
      cJSON_Delete(image);
      valid = false;
    }
  if (!cJSON_AddItemToArray(messages, message))
    {
      cJSON_Delete(message);
      valid = false;
    }
  cJSON_Delete(args);
  char *response = valid ? malloc(128 * 1024) : NULL;
  ret = !response ? -ENOMEM
                  : ny_mimo_request(&settings, body, response, 128 * 1024,
                                    NULL, NULL);
  if (ret == 0)
    {
      cJSON *root = cJSON_Parse(response);
      cJSON *choice = cJSON_GetArrayItem(
          cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
      const char *answer = ny_mimo_string(
          cJSON_GetObjectItemCaseSensitive(choice, "message"), "content");
      ret = !answer[0] ? -EBADMSG : strlen(answer) >= capacity ? -E2BIG : 0;
      if (ret == 0)
        snprintf(output, capacity, "%s", answer);
      cJSON_Delete(root);
    }
  if (ret < 0)
    snprintf(output, capacity, "MiMo image request failed (%d)", ret);
  free(response);
  cJSON_Delete(body);
  memset(&settings, 0, sizeof(settings));
  return ret;
}
