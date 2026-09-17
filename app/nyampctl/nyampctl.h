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

#endif /* __APP_NYAMPCTL_NYAMPCTL_H */
