/****************************************************************************
 * app/nyampctl/nyampctl.h
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

#ifndef __APP_NYAMPCTL_NYAMPCTL_H
#define __APP_NYAMPCTL_NYAMPCTL_H

/****************************************************************************
 * The shared client surface for the AMP compute service.  The endpoint is
 * owned by nyampctl_main.c; these entry points borrow the open descriptor so
 * every subcommand shares one discovery and bind path.
 *
 * When the Nyabula Core compute service is running it owns the endpoint
 * instead, the command goes through one of its ports, and `fd` is -1; the
 * helpers in nyampctl_io.h hide the difference.
 *
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/* Query the health or info opcode on an already bound endpoint. */
int nyampctl_query(int fd, uint16_t opcode);

int nyampctl_llm_load(int fd, const char *directory);
int nyampctl_llm_unload(int fd);

/* Generate from a token-id file, or from comma separated ids directly when
 * `inline_ids` is set (a minimal profile may have no filesystem at all).
 */
int nyampctl_llm_generate(int fd, const char *source, uint32_t max_new_tokens,
                          bool inline_ids);

/* One chat completion: `path` holds an OpenAI chat-completions request
 * ({"messages":[...],"tools":[...]}).  Prints the chat.completion response
 * and the token counts and timings.  max_new_tokens 0 selects the daemon's
 * default.
 */
int nyampctl_llm_chat(int fd, const char *path, uint32_t max_new_tokens);

/* Ask the compute domain to measure the link: `rounds` message round trips,
 * then pattern fills of a `window_bytes` window of the shared arena (0 = the
 * whole granted slot).  These are the numbers the blob window size has to be
 * tuned against, and they can only be taken on a board.
 */
int nyampctl_blob_bench(int fd, uint32_t rounds, uint32_t window_bytes);

/* Ask the compute domain to pull `name` (a file or a directory under
 * /data/models) into its tmpfs, exactly as a model load would, without
 * loading anything.  Requires the Core compute service on this side: it is
 * what answers the pull.
 */
int nyampctl_blob_pull(int fd, const char *name);

/* The voice chain, one link at a time (nyampctl_voice.c); only with
 * NYABULA_CORE_VOICE.  Each loads the model it needs by its logical name.
 * `seconds` bounds the microphone commands; `wav` NULL means the microphone;
 * `out` NULL means the speaker, else a 44.1 kHz mono WAV file is written.
 */
int nyampctl_kws_listen(int fd, unsigned int seconds);
int nyampctl_asr(int fd, const char *wav, unsigned int seconds);
int nyampctl_tts_say(int fd, const char *text, const char *out);

/* Print the Core compute service's view of the link.  Fails with -ENOSYS in
 * a build that does not contain that service.
 */
int nyampctl_status(void);

/* Write a pattern across the shared region and read it back, so a size or
 * mapping disagreement with the compute domain is caught here rather than
 * surfacing later as corrupted audio.  With `keep` set the pattern is left in
 * place for the peer to inspect; otherwise the region is cleared.
 */
int nyampctl_shmem_test(bool keep);

#endif /* __APP_NYAMPCTL_NYAMPCTL_H */
