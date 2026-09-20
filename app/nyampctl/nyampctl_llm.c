/****************************************************************************
 * app/nyampctl/nyampctl_llm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
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

/****************************************************************************
 * The openvela side of the LLM compute service.
 *
 * A generate request carries its token array in consecutive chunks on one
 * endpoint, then the service streams token events back until a terminal
 * finish event arrives.  This client must therefore write every chunk before
 * it waits, and must keep reading until the terminal event whose request_id
 * matches its own; any other event is either an older request's tail or a
 * protocol error.
 *
 ****************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nyamp_protocol.h"
#include "nyampctl.h"
#include "nyampctl_io.h"

#define NYAMPCTL_LLM_TIMEOUT_MS 60000
#define NYAMPCTL_LLM_POLL_MS    1000

/* A LOAD by logical name first pulls the model out of /data/models, and the
 * first pull of a file also hashes it: 875 MB is a few minutes end to end.
 */

#define NYAMPCTL_LLM_LOAD_TIMEOUT_MS 600000

static uint32_t nyampctl_llm_get_le32(const uint8_t *source)
{
  uint32_t value = 0;
  unsigned int index;

  for (index = 0; index < 4; index++)
    {
      value |= (uint32_t)source[index] << (index * 8);
    }

  return value;
}

static uint64_t nyampctl_llm_request_id(void) { return nyampctl_request_id(); }

static int nyampctl_llm_write(int fd, const uint8_t *wire, size_t size)
{
  return nyampctl_send(fd, wire, size);
}

/****************************************************************************
 * Name: nyampctl_llm_expect_response
 *
 * Description:
 *   Read frames until the response to `request_id` arrives.  Token events for
 *   a previous request may still be in flight, so they are printed and
 *   skipped rather than mistaken for the answer.  Progress events of a model
 *   pull that a LOAD set off are shown, since that wait can be minutes.
 *
 ****************************************************************************/

static int nyampctl_llm_expect_response(int fd, uint64_t request_id,
                                        struct nyamp_header_s *response,
                                        uint8_t *wire, size_t wire_size,
                                        size_t *wire_length, int timeout_ms)
{
  int waited = 0;

  while (waited < timeout_ms)
    {
      ssize_t size;
      struct nyamp_header_s header;
      struct nyamp_blob_progress_s progress;

      size = nyampctl_recv(fd, wire, wire_size, NYAMPCTL_LLM_POLL_MS);
      if (size < 0)
        {
          return (int)size;
        }

      if (size == 0)
        {
          waited += NYAMPCTL_LLM_POLL_MS;
          continue;
        }

      if (nyamp_header_decode(&header, wire, (size_t)size) != NYAMP_OK)
        {
          return -EPROTO;
        }

      if (header.flags == NYAMP_FLAG_EVENT &&
          header.service == NYAMP_SERVICE_BLOB &&
          header.opcode == NYAMP_BLOB_EVENT_PROGRESS &&
          header.request_id == request_id &&
          nyamp_blob_progress_decode(&progress, wire + NYAMP_WIRE_HEADER_SIZE,
                                     header.payload_size) == NYAMP_OK)
        {
          printf("nyamp llm: pulling model %" PRIu64 "/%" PRIu64
                 " bytes, %" PRIu32 " KiB/s\n",
                 progress.done, progress.total,
                 progress.bytes_per_second / 1024);
          continue;
        }

      /* A failed response also sets NYAMP_FLAG_ERROR, so the kind must be
       * compared with the error bit masked off.
       */
      if ((header.flags & NYAMP_FLAG_KIND_MASK) == NYAMP_FLAG_RESPONSE &&
          header.request_id == request_id)
        {
          *response = header;
          *wire_length = (size_t)size;
          return 0;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: nyampctl_llm_drain_until_finish
 *
 * Description:
 *   Print token events and stop at the terminal finish event for this request.
 *   A finish carrying a non-zero status reports the failure that ended the
 *   generation; success is only claimed for status zero.
 *
 ****************************************************************************/

static int nyampctl_llm_drain_until_finish(int fd, uint64_t request_id)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  int waited = 0;
  uint32_t tokens = 0;

  printf("nyamp llm: generating");
  fflush(stdout);

  while (waited < NYAMPCTL_LLM_TIMEOUT_MS)
    {
      ssize_t size;
      struct nyamp_header_s header;

      size = nyampctl_recv(fd, wire, sizeof(wire), NYAMPCTL_LLM_POLL_MS);
      if (size < 0)
        {
          return (int)size;
        }

      if (size == 0)
        {
          waited += NYAMPCTL_LLM_POLL_MS;
          continue;
        }

      if (nyamp_header_decode(&header, wire, (size_t)size) != NYAMP_OK ||
          header.flags != NYAMP_FLAG_EVENT || header.request_id != request_id)
        {
          continue;
        }

      if (header.opcode == NYAMP_LLM_EVENT_TOKEN)
        {
          uint32_t token_id;
          uint32_t sequence;
          const char *text;
          size_t text_length;

          if (nyamp_llm_token_decode(&token_id, &sequence, &text, &text_length,
                                     wire + NYAMP_WIRE_HEADER_SIZE,
                                     header.payload_size) != NYAMP_OK)
            {
              return -EPROTO;
            }

          fwrite(text, 1, text_length, stdout);
          fflush(stdout);
          tokens++;
          continue;
        }

      if (header.opcode == NYAMP_LLM_EVENT_FINISH)
        {
          int32_t status;
          uint32_t sequence;

          if (nyamp_llm_finish_decode(&status, &sequence,
                                      wire + NYAMP_WIRE_HEADER_SIZE,
                                      header.payload_size) != NYAMP_OK)
            {
              return -EPROTO;
            }

          printf("\nnyamp llm: finish status=%" PRId32 " tokens=%" PRIu32 "\n",
                 status, tokens);
          return status == NYAMP_MODEL_OK ? 0 : -EREMOTEIO;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: nyampctl_llm_read_ids
 *
 * Description:
 *   Read whitespace separated non-negative decimal token ids from a file.
 *   The array arrives already tokenized: this client does not implement a
 *   tokenizer and must not pretend to.
 *
 ****************************************************************************/

static int nyampctl_llm_read_ids(const char *path, int32_t *ids,
                                 size_t capacity, size_t *count)
{
  FILE *file = fopen(path, "r");
  size_t used = 0;

  if (file == NULL)
    {
      return -errno;
    }

  for (;;)
    {
      long value;

      if (fscanf(file, "%ld", &value) != 1)
        {
          break;
        }

      if (value < 0 || value > INT32_MAX || used >= capacity)
        {
          fclose(file);
          return -EINVAL;
        }

      ids[used++] = (int32_t)value;
    }

  fclose(file);
  *count = used;
  return used == 0 ? -EINVAL : 0;
}

/****************************************************************************
 * Name: nyampctl_llm_parse_ids
 *
 * Description:
 *   Parse comma or space separated ids from a single command line argument.
 *   A minimal AMP profile may have no filesystem at all, and the product
 *   domain already holds the token array in memory, so the array must be
 *   acceptable without a file on disk.
 *
 ****************************************************************************/

static int nyampctl_llm_parse_ids(const char *text, int32_t *ids,
                                  size_t capacity, size_t *count)
{
  size_t used = 0;
  const char *cursor = text;

  while (*cursor != '\0')
    {
      char *end = NULL;
      long value;

      while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
        {
          ++cursor;
        }

      if (*cursor == '\0')
        {
          break;
        }

      value = strtol(cursor, &end, 10);
      if (end == cursor || value < 0 || value > INT32_MAX || used >= capacity)
        {
          return -EINVAL;
        }

      ids[used++] = (int32_t)value;
      cursor = end;
    }

  *count = used;
  return used == 0 ? -EINVAL : 0;
}

static int nyampctl_llm_request(int fd, uint16_t opcode, uint64_t request_id,
                                const uint8_t *body, size_t body_size)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s request = {
    .service = NYAMP_SERVICE_LLM,
    .opcode = opcode,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = request_id,
    .deadline_ms = 0,
    .generation = 0,
    .payload_size = (uint32_t)body_size,
  };
  struct nyamp_header_s response = { 0 };
  size_t length = 0;
  int32_t status;
  int ret;

  ret = nyamp_header_encode(wire, sizeof(wire), &request);
  if (ret != NYAMP_OK || body_size > NYAMP_INLINE_MAX)
    {
      return ret != NYAMP_OK ? ret : -EMSGSIZE;
    }

  if (body_size != 0)
    {
      memcpy(wire + NYAMP_WIRE_HEADER_SIZE, body, body_size);
    }

  ret = nyampctl_llm_write(fd, wire, NYAMP_WIRE_HEADER_SIZE + body_size);
  if (ret < 0)
    {
      return ret;
    }

  ret = nyampctl_llm_expect_response(
      fd, request_id, &response, wire, sizeof(wire), &length,
      opcode == NYAMP_LLM_LOAD ? NYAMPCTL_LLM_LOAD_TIMEOUT_MS
                               : NYAMPCTL_LLM_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (response.service != NYAMP_SERVICE_LLM || response.opcode != opcode ||
      response.payload_size < 4)
    {
      return -EPROTO;
    }

  status = (int32_t)nyampctl_llm_get_le32(wire + NYAMP_WIRE_HEADER_SIZE);
  if (status != NYAMP_MODEL_OK)
    {
      fprintf(stderr, "nyampctl: llm opcode %u failed: status=%" PRId32 "\n",
              opcode, status);
      return -EREMOTEIO;
    }

  return 0;
}

int nyampctl_llm_load(int fd, const char *directory)
{
  return nyampctl_llm_request(fd, NYAMP_LLM_LOAD, nyampctl_llm_request_id(),
                              (const uint8_t *)directory, strlen(directory));
}

int nyampctl_llm_unload(int fd)
{
  return nyampctl_llm_request(fd, NYAMP_LLM_UNLOAD, nyampctl_llm_request_id(),
                              NULL, 0);
}

/****************************************************************************
 * Name: nyampctl_llm_generate
 *
 * Description:
 *   Send a token array as ordered chunks, then read the streamed events.  The
 *   response to each chunk is consumed before the next chunk is written, so
 *   the endpoint never holds two requests at once.
 *
 *   `source` is either a file path or, with `inline_ids` set, the array
 *   itself as comma separated decimal ids.
 *
 ****************************************************************************/

int nyampctl_llm_generate(int fd, const char *source, uint32_t max_new_tokens,
                          bool inline_ids)
{
  static int32_t ids[NYAMP_LLM_MAX_CHUNK_IDS * 16];
  size_t total = 0;
  size_t offset;
  uint64_t request_id = nyampctl_llm_request_id();
  int ret;

  if (inline_ids)
    {
      ret = nyampctl_llm_parse_ids(source, ids, sizeof(ids) / sizeof(ids[0]),
                                   &total);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: cannot parse inline token ids: %d\n",
                  -ret);
          return ret;
        }
    }
  else
    {
      ret = nyampctl_llm_read_ids(source, ids, sizeof(ids) / sizeof(ids[0]),
                                  &total);
      if (ret < 0)
        {
          fprintf(stderr, "nyampctl: cannot read token ids from %s: %d\n",
                  source, -ret);
          return ret;
        }
    }

  for (offset = 0; offset < total;)
    {
      uint8_t body[NYAMP_INLINE_MAX];
      size_t body_size = 0;
      size_t count = total - offset;
      struct nyamp_llm_chunk_s chunk;

      if (count > NYAMP_LLM_MAX_CHUNK_IDS)
        {
          count = NYAMP_LLM_MAX_CHUNK_IDS;
        }

      chunk.total = (uint32_t)total;
      chunk.offset = (uint32_t)offset;
      chunk.count = (uint32_t)count;
      chunk.max_new_tokens = offset == 0 ? max_new_tokens : 0;

      ret = nyamp_llm_chunk_encode(body, sizeof(body), &body_size, &chunk,
                                   ids + offset);
      if (ret != NYAMP_OK)
        {
          return ret;
        }

      /* Every chunk shares the request id so the service can bind them. */
      ret = nyampctl_llm_request(fd, NYAMP_LLM_GENERATE, request_id, body,
                                 body_size);
      if (ret < 0)
        {
          return ret;
        }

      offset += count;
    }

  return nyampctl_llm_drain_until_finish(fd, request_id);
}
