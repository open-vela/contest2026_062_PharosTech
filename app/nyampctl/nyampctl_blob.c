/****************************************************************************
 * app/nyampctl/nyampctl_blob.c
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
 * Driving model delivery from the control-domain shell.
 *
 * In the BLOB service the compute domain is the requester: it pulls model
 * files out of /data/models.  It also has no console, so the only way to
 * exercise or measure that path without loading a model is to ask it to.
 * BENCH_RUN and PULL are those two requests; they travel in the ordinary
 * direction and the compute domain answers them when the work is done.
 *
 * Both need something on this side to answer the blob requests they set
 * off, which is the Nyabula Core compute service, not this diagnostic.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nyamp_protocol.h"
#include "nyampctl.h"
#include "nyampctl_io.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NYAMPCTL_BLOB_POLL_MS 1000

/* 200 round trips and 32 one-megabyte fills finish in well under this. */

#define NYAMPCTL_BENCH_TIMEOUT_MS 120000

/* An 875 MB pull that also has to hash the file first. */

#define NYAMPCTL_PULL_TIMEOUT_MS 900000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int nyampctl_blob_run(int fd, uint16_t opcode, const uint8_t *body,
                             size_t body_size, int timeout_ms, uint8_t *answer,
                             size_t answer_capacity, size_t *answer_size);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nyampctl_blob_run
 *
 * Description:
 *   Send one control-originated BLOB request and wait for its response,
 *   printing the progress events that arrive under the same request id.
 *   On success the response body (after the status) is copied out.
 *
 ****************************************************************************/

static int nyampctl_blob_run(int fd, uint16_t opcode, const uint8_t *body,
                             size_t body_size, int timeout_ms, uint8_t *answer,
                             size_t answer_capacity, size_t *answer_size)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s request;
  int waited = 0;
  int ret;

  memset(&request, 0, sizeof(request));
  request.service = NYAMP_SERVICE_BLOB;
  request.opcode = opcode;
  request.flags = NYAMP_FLAG_REQUEST;
  request.request_id = nyampctl_request_id();
  request.payload_size = (uint32_t)body_size;

  ret = nyamp_header_encode(wire, sizeof(wire), &request);
  if (ret != NYAMP_OK)
    {
      return ret;
    }

  memcpy(wire + NYAMP_WIRE_HEADER_SIZE, body, body_size);
  ret = nyampctl_send(fd, wire, NYAMP_WIRE_HEADER_SIZE + body_size);
  if (ret < 0)
    {
      return ret;
    }

  while (waited < timeout_ms)
    {
      struct nyamp_blob_progress_s progress;
      struct nyamp_header_s header;
      const uint8_t *payload = wire + NYAMP_WIRE_HEADER_SIZE;
      const uint8_t *data;
      size_t data_size;
      int32_t status;
      ssize_t size;

      size = nyampctl_recv(fd, wire, sizeof(wire), NYAMPCTL_BLOB_POLL_MS);
      if (size < 0)
        {
          return (int)size;
        }

      if (size == 0)
        {
          waited += NYAMPCTL_BLOB_POLL_MS;
          continue;
        }

      if (nyamp_header_decode(&header, wire, (size_t)size) != NYAMP_OK ||
          header.request_id != request.request_id ||
          header.service != NYAMP_SERVICE_BLOB)
        {
          continue;
        }

      if (header.flags == NYAMP_FLAG_EVENT)
        {
          if (header.opcode == NYAMP_BLOB_EVENT_PROGRESS &&
              nyamp_blob_progress_decode(&progress, payload,
                                         header.payload_size) == NYAMP_OK)
            {
              printf("nyamp blob: %" PRIu64 "/%" PRIu64 " bytes, %" PRIu32
                     " KiB/s\n",
                     progress.done, progress.total,
                     progress.bytes_per_second / 1024);
            }

          continue;
        }

      if ((header.flags & NYAMP_FLAG_KIND_MASK) != NYAMP_FLAG_RESPONSE ||
          header.opcode != opcode ||
          nyamp_status_decode(&status, &data, &data_size, payload,
                              header.payload_size) != NYAMP_OK)
        {
          return -EPROTO;
        }

      if (status != NYAMP_MODEL_OK)
        {
          fprintf(stderr,
                  "nyampctl: blob opcode 0x%x failed: status=%" PRId32 "\n",
                  opcode, status);
          return -EREMOTEIO;
        }

      if (data_size > answer_capacity)
        {
          return -EMSGSIZE;
        }

      memcpy(answer, data, data_size);
      *answer_size = data_size;
      return 0;
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int nyampctl_blob_bench(int fd, uint32_t rounds, uint32_t window_bytes)
{
  struct nyamp_blob_bench_report_s report;
  uint8_t body[NYAMP_BLOB_BENCH_RUN_SIZE];
  uint8_t answer[NYAMP_BLOB_BENCH_REPORT_SIZE];
  size_t body_size = 0;
  size_t answer_size = 0;
  int ret;

  ret = nyamp_blob_bench_run_encode(body, sizeof(body), &body_size, rounds,
                                    window_bytes);
  if (ret != NYAMP_OK)
    {
      return ret;
    }

  ret = nyampctl_blob_run(fd, NYAMP_BLOB_BENCH_RUN, body, body_size,
                          NYAMPCTL_BENCH_TIMEOUT_MS, answer, sizeof(answer),
                          &answer_size);
  if (ret < 0)
    {
      return ret;
    }

  if (nyamp_blob_bench_report_decode(&report, answer, answer_size) != NYAMP_OK)
    {
      return -EPROTO;
    }

  printf("nyamp blob bench: rounds=%" PRIu32 " rtt_us min=%" PRIu32
         " avg=%" PRIu32 " max=%" PRIu32 "\n",
         report.rounds, report.rtt_min_us, report.rtt_avg_us,
         report.rtt_max_us);

  if (report.window_bytes == 0)
    {
      printf("nyamp blob bench: no shared window on the compute side\n");
      return 0;
    }

  /* "fill" is the control domain writing the window, one round trip
   * included, which is what a real READ costs minus the eMMC.  "copy" is the
   * compute domain lifting the window into ordinary memory.
   */

  printf("nyamp blob bench: window=%" PRIu32 " fill=%" PRIu32
         " KiB/s copy=%" PRIu32 " KiB/s pattern_errors=%" PRIu32 "\n",
         report.window_bytes, report.fill_kib_per_s, report.copy_kib_per_s,
         report.pattern_errors);

  /* Errors here mean the two domains do not see the same bytes -- a stale or
   * cacheable mapping -- and every model pull would fail its digest.
   */

  return report.pattern_errors == 0 ? 0 : -EIO;
}

int nyampctl_blob_pull(int fd, const char *name)
{
  struct nyamp_blob_pull_report_s report;
  uint8_t body[NYAMP_INLINE_MAX];
  uint8_t answer[NYAMP_BLOB_PULL_REPORT_SIZE];
  size_t body_size = 0;
  size_t answer_size = 0;
  int ret;

  /* PULL carries a name the same way OPEN does. */

  ret = nyamp_blob_open_encode(body, sizeof(body), &body_size, 0, name,
                               strlen(name));
  if (ret != NYAMP_OK)
    {
      fprintf(stderr, "nyampctl: not a blob name (relative, no \"..\"): %s\n",
              name);
      return -EINVAL;
    }

  ret = nyampctl_blob_run(fd, NYAMP_BLOB_PULL, body, body_size,
                          NYAMPCTL_PULL_TIMEOUT_MS, answer, sizeof(answer),
                          &answer_size);
  if (ret < 0)
    {
      return ret;
    }

  if (nyamp_blob_pull_report_decode(&report, answer, answer_size) != NYAMP_OK)
    {
      return -EPROTO;
    }

  printf("nyamp blob pull: %s files=%" PRIu32 " reused=%" PRIu32
         " bytes=%" PRIu64 " ms=%" PRIu64,
         name, report.files, report.reused, report.bytes, report.elapsed_ms);
  if (report.elapsed_ms != 0 && report.bytes != 0)
    {
      printf(" (%" PRIu64 " KiB/s)",
             report.bytes * 1000 / report.elapsed_ms / 1024);
    }

  printf("\n");
  return 0;
}

int nyampctl_status(void)
{
#ifdef CONFIG_NYABULA_CORE_COMPUTE
  struct ny_compute_status_s status;
  int ret = ny_compute_status(&status);

  if (ret < 0)
    {
      return ret;
    }

  printf("nyamp compute: running=%d linked=%d generation=%" PRIu32
         " capabilities=0x%08" PRIx32 "\n",
         status.running, status.linked, status.generation,
         status.capabilities);
  printf("nyamp compute: blob active=%d hashing=%d name=%s offset=%" PRIu64
         " size=%" PRIu64 " rate=%" PRIu32 " B/s\n",
         status.blob_active, status.blob_hashing, status.blob_name,
         status.blob_offset, status.blob_size, status.blob_bytes_per_sec);
  printf("nyamp compute: generation_changes=%" PRIu32 " dropped=%" PRIu32
         " last_error=%d %s\n",
         status.generation_changes, status.dropped_frames, status.last_error,
         status.last_error_text);
  return 0;
#else
  fprintf(stderr, "nyampctl: built without the Core compute service\n");
  return -ENOSYS;
#endif
}
