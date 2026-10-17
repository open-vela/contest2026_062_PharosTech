/****************************************************************************
 * app/nyabula_core/ny_compute.h
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

#ifndef __NYABULA_CORE_NY_COMPUTE_H
#define __NYABULA_CORE_NY_COMPUTE_H

/****************************************************************************
 * The control domain's link to the Linux compute domain.
 *
 * One task owns the read side of the one RPMsg endpoint for as long as the
 * product runs.  That is forced by the transport, not chosen: the endpoint
 * has a single receive queue, so two readers would steal each other's
 * frames, and the compute domain now sends REQUESTS of its own (the BLOB
 * service, which pulls model files out of /data/models) that someone has to
 * be listening for at all times.
 *
 * Everything else talks through this library:
 *
 *  - the receive task answers inbound BLOB requests itself;
 *  - a local requester opens a PORT, sends frames through it and receives
 *    the responses and events that carry its request_id.
 *
 * This header deliberately has no cJSON or product types in it, so the
 * nyampctl diagnostic can include it; the panel topic entry point lives in
 * ny_product.h with the other topic handlers.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef CONFIG_NYABULA_CORE_COMPUTE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Blob names resolve strictly under this directory. */

#define NY_COMPUTE_BLOB_ROOT   "/data/models"

#define NY_COMPUTE_NAME_MAX    255
#define NY_COMPUTE_ERROR_MAX   64

/* Bits of the capability mask the compute domain reports in HEALTH. */

#define NY_COMPUTE_CAP_HEALTH  (1U << 0)
#define NY_COMPUTE_CAP_LLM     (1U << 1)
#define NY_COMPUTE_CAP_BLOB    (1U << 2)
#define NY_COMPUTE_CAP_CHAT    (1U << 3)
#define NY_COMPUTE_CAP_ASR     (1U << 4)
#define NY_COMPUTE_CAP_TTS     (1U << 5)
#define NY_COMPUTE_CAP_KWS     (1U << 6)

/* ny_compute_chat() flags; the values are the wire's NYAMP_LLM_CHAT_*. */

#define NY_COMPUTE_CHAT_GUARD_UNTRUSTED (1U << 0)
#define NY_COMPUTE_CHAT_STREAM_TOKENS   (1U << 1)

/* The largest request body a chat may carry. */

#define NY_COMPUTE_CHAT_MAX_REQUEST 65536

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_compute_port_s;

/* The local model as this domain knows it.  PROVISIONING and LOADING are the
 * two halves of a load: the compute domain pulling the model out of
 * /data/models, then its runtime initialising from the copy.
 */

enum ny_compute_llm_state_e
{
  NY_COMPUTE_LLM_UNLOADED = 0,
  NY_COMPUTE_LLM_PROVISIONING,
  NY_COMPUTE_LLM_LOADING,
  NY_COMPUTE_LLM_READY,
  NY_COMPUTE_LLM_BUSY,
  NY_COMPUTE_LLM_ERROR
};

struct ny_compute_chat_stats_s
{
  uint32_t prompt_tokens;
  uint32_t completion_tokens;
  uint32_t prefill_ms;    /* Request accepted to first token.           */
  uint32_t decode_ms;     /* First token to last token.                 */
  uint32_t context_limit; /* What prompt + max_new_tokens has to fit.   */
};

struct ny_compute_status_s
{
  bool running;          /* The receive task exists.                      */
  bool linked;           /* A frame arrived recently and the generation   */
                         /* is known.                                     */
  uint32_t generation;   /* Compute-domain generation, 0 when unknown.    */
  uint32_t capabilities; /* From the last HEALTH response.                */

  bool blob_active;      /* A blob is open or being hashed.               */
  bool blob_hashing;     /* OPEN is still computing the digest.           */
  char blob_name[NY_COMPUTE_NAME_MAX + 1];
  uint64_t blob_offset;  /* End of the last window served (or hashed).    */
  uint64_t blob_size;
  uint32_t blob_bytes_per_sec;

  enum ny_compute_llm_state_e llm_state;
  char llm_model[NY_COMPUTE_NAME_MAX + 1];
  struct ny_compute_chat_stats_s llm_last; /* The most recent chat.       */
  uint32_t llm_tokens_per_sec_x10;         /* Decode rate of that chat.   */
  int llm_last_error;
  char llm_last_error_text[NY_COMPUTE_ERROR_MAX];

  uint32_t generation_changes;
  uint32_t dropped_frames; /* Malformed, or a port queue was full.        */
  int last_error;          /* Negated errno, 0 when none.                 */
  char last_error_text[NY_COMPUTE_ERROR_MAX];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: ny_compute_start / ny_compute_stop
 *
 * Description:
 *   Start or stop the receive task.  Start is idempotent.  The task keeps
 *   retrying while the AMP transport is absent, so it is safe to start on a
 *   firmware that booted without the compute domain.
 *
 ****************************************************************************/

int ny_compute_start(void);
int ny_compute_stop(void);
bool ny_compute_running(void);

int ny_compute_status(struct ny_compute_status_s *status);

/****************************************************************************
 * Name: ny_compute_generation / ny_compute_capabilities
 *
 * Description:
 *   The two facts of ny_compute_status() a requester asks for on every
 *   frame, without the copy of the rest: the compute-domain generation (0
 *   while unknown; a change means the daemon restarted and every grant,
 *   lease and loaded model of the old one is gone) and the NY_COMPUTE_CAP_*
 *   mask of the last HEALTH response.
 *
 ****************************************************************************/

uint32_t ny_compute_generation(void);
uint32_t ny_compute_capabilities(void);

/****************************************************************************
 * Name: ny_compute_endpoint_open
 *
 * Description:
 *   Create the `rpmsg-raw` endpoint if it does not exist yet, wait for its
 *   device node and open it.  Returns a descriptor or a negated errno.  This
 *   is the discovery and bind sequence the nyampctl diagnostic used to carry
 *   on its own.
 *
 ****************************************************************************/

int ny_compute_endpoint_open(int oflags);

/****************************************************************************
 * Name: ny_compute_port_open / ny_compute_port_close
 *
 * Description:
 *   A port is one local requester's view of the link.  It may be opened from
 *   any task: frames are written through a descriptor that belongs to the
 *   caller (descriptors are per task, the receive task's cannot be borrowed)
 *   and received from a queue the receive task fills.
 *
 *   Returns -ENOTCONN when the receive task is not running, in which case
 *   the caller may use the endpoint directly as nyampctl does standalone.
 *
 ****************************************************************************/

int ny_compute_port_open(struct ny_compute_port_s **port);
void ny_compute_port_close(struct ny_compute_port_s *port);

/****************************************************************************
 * Name: ny_compute_port_send
 *
 * Description:
 *   Send one encoded frame.  The port remembers the frame's request_id and
 *   from then on receives the responses and events that carry it; frames for
 *   other requesters are never delivered here.
 *
 ****************************************************************************/

int ny_compute_port_send(struct ny_compute_port_s *port, const uint8_t *wire,
                         size_t size);

/****************************************************************************
 * Name: ny_compute_port_recv
 *
 * Description:
 *   Receive the next frame for this port.  Returns the frame size, 0 on
 *   timeout, or a negated errno.
 *
 ****************************************************************************/

ssize_t ny_compute_port_recv(struct ny_compute_port_s *port, uint8_t *wire,
                             size_t capacity, int timeout_ms);

/****************************************************************************
 * Name: ny_compute_request_id
 *
 * Description:
 *   A request id that is unique across every task of this domain and never
 *   carries NYAMP_REQUEST_ID_COMPUTE.
 *
 ****************************************************************************/

uint64_t ny_compute_request_id(void);

/****************************************************************************
 * Name: ny_compute_llm_load / ny_compute_llm_unload
 *
 * Description:
 *   Load a model on the compute domain, or drop it.  `name` is a logical
 *   name under /data/models ("llm/model.rkllm"): the compute domain pulls the
 *   file and its tokenizer.json through the BLOB service this library
 *   serves, so the first load of an 875 MB model is a matter of minutes.
 *   NULL selects CONFIG_NYABULA_CORE_COMPUTE_LLM.  Loading what is already
 *   loaded succeeds at once.
 *
 *   Blocking.  -EBUSY while another load, unload or chat is in progress,
 *   -ENOENT when the model or its tokenizer is not on this device,
 *   -ETIMEDOUT, -ENOTCONN without a link.  timeout_ms <= 0 selects a default
 *   sized for a cold load.
 *
 ****************************************************************************/

int ny_compute_llm_load(const char *name, int timeout_ms);
int ny_compute_llm_unload(void);

/****************************************************************************
 * Name: ny_compute_chat
 *
 * Description:
 *   One chat completion on the local model.  `request_json` is an OpenAI
 *   chat-completions request ({"messages":[...],"tools":[...]}); on success
 *   `*response_json` is a malloc'd, NUL terminated chat.completion object the
 *   caller frees.  Tokenizing, the chat template and tool-call parsing all
 *   happen on the compute domain; this side only moves text.
 *
 *   If no model is loaded, CONFIG_NYABULA_CORE_COMPUTE_LLM is loaded first,
 *   so the first call after boot can take minutes (see timeout_ms).
 *
 *   Blocking, one at a time.  Returns 0, or:
 *     -EBUSY      another chat, load or unload is in progress
 *     -E2BIG      prompt + max_new_tokens does not fit the context window;
 *                 stats->prompt_tokens and stats->context_limit say by how
 *                 much, so the caller can drop history and try again
 *     -ECANCELED  ny_compute_chat_cancel() was called
 *     -ETIMEDOUT  no terminal event within timeout_ms (<= 0: a default)
 *     -EMSGSIZE   the request exceeds NY_COMPUTE_CHAT_MAX_REQUEST
 *     -EINVAL     the compute domain could not read the request
 *     -ENOTSUP    the loaded model has no tokenizer, or the daemon no chat
 *     -ENOENT     the model is not on this device
 *     -ENOTCONN / -ECONNRESET  no link, or the compute domain restarted
 *   `stats` may be NULL.  max_new_tokens 0 selects the daemon's default.
 *
 ****************************************************************************/

int ny_compute_chat(const char *request_json, size_t max_new_tokens,
                    unsigned int flags, char **response_json,
                    struct ny_compute_chat_stats_s *stats, int timeout_ms);

/****************************************************************************
 * Name: ny_compute_chat_cancel
 *
 * Description:
 *   Stop the chat in progress, from any thread.  The blocked
 *   ny_compute_chat() returns -ECANCELED once the compute domain confirms.
 *   -ENOENT when no chat is running.
 *
 ****************************************************************************/

int ny_compute_chat_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_NYABULA_CORE_COMPUTE */
#endif /* __NYABULA_CORE_NY_COMPUTE_H */
